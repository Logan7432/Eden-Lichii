// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include "common/assert.h"
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/hle/service/friend/resort_lan_friends.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/room_source.h"
#include "core/internal_network/socket_proxy.h"
#include "network/network.h"

#ifdef __unix__
#include <sys/socket.h>
#endif

namespace Network {

namespace {

u16 InternetChecksum(std::span<const u8> data) {
    u32 sum = 0;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        sum += static_cast<u32>(data[i] << 8 | data[i + 1]);
    }
    if (data.size() % 2 != 0) {
        sum += static_cast<u32>(data.back() << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<u16>(~sum);
}

std::vector<u8> MakeIcmpEchoReply(std::span<const u8> request, const IPv4Address& from,
                                  const IPv4Address& to) {
    constexpr std::size_t IpHeaderSize = 20;
    std::vector<u8> packet(IpHeaderSize + request.size());
    const auto total_length = static_cast<u16>(packet.size());
    packet[0] = 0x45;
    packet[2] = static_cast<u8>(total_length >> 8);
    packet[3] = static_cast<u8>(total_length);
    packet[8] = 64;
    packet[9] = 1;
    std::copy(from.begin(), from.end(), packet.begin() + 12);
    std::copy(to.begin(), to.end(), packet.begin() + 16);
    const u16 header_checksum = InternetChecksum(std::span(packet).first(IpHeaderSize));
    packet[10] = static_cast<u8>(header_checksum >> 8);
    packet[11] = static_cast<u8>(header_checksum);

    std::copy(request.begin(), request.end(), packet.begin() + IpHeaderSize);
    packet[IpHeaderSize] = 0;
    packet[IpHeaderSize + 1] = 0;
    packet[IpHeaderSize + 2] = 0;
    packet[IpHeaderSize + 3] = 0;
    const u16 icmp_checksum = InternetChecksum(std::span(packet).subspan(IpHeaderSize));
    packet[IpHeaderSize + 2] = static_cast<u8>(icmp_checksum >> 8);
    packet[IpHeaderSize + 3] = static_cast<u8>(icmp_checksum);
    return packet;
}

} // Anonymous namespace

namespace {
struct RoomBinding {
    const ProxySocket* owner;
    Protocol protocol;
    SockAddrIn endpoint;
    bool reuse_addr;
    bool reuse_port;
    bool local_identity;
    bool automatic;
};
// Always acquired after the owning socket's packets_mutex; never calls a socket.
struct RoomBindings {
    std::mutex mutex;
    std::vector<RoomBinding> entries;
    u16 next_ephemeral_port = 49152;
};

RoomBindings& GetBindings() {
    // BSD descriptors can survive service teardown. Keep the small registry alive until
    // process exit, so their late destructors never use destroyed statics from another TU.
    static auto* const registry = new RoomBindings;
    return *registry;
}

void ReleaseBinding(const ProxySocket* socket) {
    auto& registry = GetBindings();
    std::lock_guard guard(registry.mutex);
    std::erase_if(registry.entries,
                  [socket](const auto& binding) { return binding.owner == socket; });
}

bool Wildcard(const IPv4Address& ip) {
    return ip == IPv4Address{};
}
} // namespace

ProxySocket::ProxySocket(LocalAddressProvider addresses) : local_addresses{std::move(addresses)} {}

ProxySocket::~ProxySocket() {
    Close();
}

bool ProxySocket::MatchesPacket(const ProxyPacket& packet) const {
    if (closed || packet.local_endpoint.family != Domain::INET ||
        packet.remote_endpoint.family != Domain::INET || protocol != packet.protocol ||
        local_endpoint.portno != packet.remote_endpoint.portno) {
        return false;
    }
    if (!broadcast && packet.broadcast) {
        LOG_INFO(Network, "Received broadcast packet, but not configured for broadcast mode");
        return false;
    }
    return true;
}

bool ProxySocket::AcceptsProxyPacket(const ProxyPacket& packet) {
    std::lock_guard guard(packets_mutex);
    return MatchesPacket(packet);
}

void ProxySocket::HandleProxyPacket(const ProxyPacket& packet) {
    {
        // Runs on the room thread, while the game thread may bind, close or reconfigure the socket.
        // Check again: the socket may have changed since AcceptsProxyPacket.
        std::lock_guard guard(packets_mutex);
        if (!MatchesPacket(packet)) {
            return;
        }
        received_packets.push(packet);
    }
    // A MSG_PEEK reader leaves the packet queued, so every waiter must see it
    packets_cv.notify_all();
}

std::optional<PollEvents> ProxySocket::EmulatedPoll(PollEvents events) {
    std::lock_guard guard(packets_mutex);
    PollEvents revents{};
    // A queued packet reads at once. So does a closed socket, whose reads fail, and a stream,
    // whose reads report its end while nothing is queued (see Recv).
    if (closed || !received_packets.empty() || socket_type == Type::STREAM) {
        revents |= events & (PollEvents::In | PollEvents::RdNorm);
    }
    // Sends go to the room without waiting.
    revents |= events & PollEvents::Out;
    return revents;
}

template <typename T>
Errno ProxySocket::SetSockOpt(SOCKET fd_, int option, T value) {
    LOG_DEBUG(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Initialize(Domain domain, Type type, Protocol socket_protocol) {
    if (domain != Domain::INET) {
        return Errno::AFNOSUPPORT;
    }
    {
        std::lock_guard guard(packets_mutex);
        socket_type = type;
        protocol = socket_protocol;
    }
    SetSockOpt(fd, SO_TYPE, type);

    return Errno::SUCCESS;
}

std::pair<ProxySocket::AcceptResult, Errno> ProxySocket::Accept() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {AcceptResult{}, Errno::SUCCESS};
}

Errno ProxySocket::Connect(SockAddrIn addr_in) {
    if (addr_in.family == Domain::INET6) {
        return Errno::AFNOSUPPORT;
    }
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> ProxySocket::GetPeerName() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {SockAddrIn{}, Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> ProxySocket::GetSockName() {
    std::lock_guard guard(packets_mutex);
    if (closed) {
        return {SockAddrIn{}, Errno::BADF};
    }

    auto endpoint = local_endpoint;
    if (!is_bound) {
        endpoint.family = Domain::INET;
    }
    return {endpoint, Errno::SUCCESS};
}

Errno ProxySocket::Bind(SockAddrIn addr) {
    std::lock_guard guard(packets_mutex);
    if (closed) {
        return Errno::BADF;
    }
    if (is_bound) {
        return Errno::INVAL;
    }
    if (addr.family != Domain::INET) {
        return Errno::INVAL;
    }

    bool local_bind = false;
    const auto member = GetRoomMember().lock();
    const auto identity = member ? member->GetENetIdentity() : std::nullopt;
    if (member) {
        if (IsDirectLanActive() || !identity) {
            return Errno::NOTCONN;
        }
        std::vector<IPv4Address> local, peers;
        if (local_addresses) {
            local = local_addresses();
        } else {
            for (const auto& adapter : GetAvailableNetworkInterfaces(true)) {
                local.push_back(TranslateIPv4(adapter.ip_address));
            }
        }
        for (const auto& peer : member->GetMemberInformationSnapshot()) {
            if (peer.fake_ip != identity->ip)
                peers.push_back(peer.fake_ip);
        }
        local_bind = IsRoomLocalBind(addr.ip, identity->ip, local, peers);
        if (!local_bind)
            return Errno::INVAL;
    }

    auto binding_protocol = protocol;
    if (protocol == Protocol::Unspecified || protocol == Protocol::IP) {
        if (socket_type == Type::STREAM) {
            binding_protocol = Protocol::TCP;
        } else if (socket_type == Type::DGRAM) {
            binding_protocol = Protocol::UDP;
        }
    }
    auto& registry = GetBindings();
    std::lock_guard registry_guard(registry.mutex);
    const auto occupied = [&](u16 port, bool automatic) {
        bool shared_addr = reuse_addr, shared_port = reuse_port;
        for (const auto& binding : registry.entries) {
            if (binding.protocol != binding_protocol || binding.endpoint.portno != port ||
                !((local_bind && binding.local_identity) || Wildcard(addr.ip) ||
                  Wildcard(binding.endpoint.ip) || addr.ip == binding.endpoint.ip)) {
                continue;
            }
            shared_addr &= binding.reuse_addr;
            shared_port &= binding.reuse_port;
            if (automatic || binding.automatic || (!shared_addr && !shared_port))
                return true;
        }
        return false;
    };
    const u16 requested_port = addr.portno;
    if (requested_port == 0) {
        bool available = false;
        for (u32 attempts = 0; attempts < 16384; ++attempts) {
            const u16 candidate = registry.next_ephemeral_port;
            registry.next_ephemeral_port = candidate == 65535 ? 49152 : candidate + 1;
            if (!occupied(candidate, true)) {
                addr.portno = candidate;
                available = true;
                break;
            }
        }
        if (!available) {
            return Errno::ADDRINUSE;
        }
    } else if (occupied(addr.portno, false)) {
        return Errno::ADDRINUSE;
    }
    if (member) {
        const auto current = member->GetENetIdentity();
        if (!current || current->generation != identity->generation)
            return Errno::NOTCONN;
    }
    registry.entries.push_back(
        {this, binding_protocol, addr, reuse_addr, reuse_port, local_bind, requested_port == 0});
    local_endpoint = addr;
    room_local_bind = local_bind;
    is_bound = true;
    LOG_INFO(Network, "Room socket bound requested_port={} assigned_port={} protocol={}",
             requested_port, addr.portno, binding_protocol);
    return Errno::SUCCESS;
}

Errno ProxySocket::Listen(s32 backlog) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Shutdown(ShutdownHow how) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

std::pair<s32, Errno> ProxySocket::Recv(int flags, std::span<u8> message) {
    u32 msg_flags = 0;
    return RecvMsg(flags, message, nullptr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    u32 msg_flags = 0;
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    msg_flags = 0;
    {
        // Room TCP sockets have no peer (Connect and Send are stubs). Report an empty queue as the
        // end of the stream, as before, instead of blocking the bsd service until the timeout.
        std::lock_guard guard(packets_mutex);
        if (closed) {
            return {-1, Errno::BADF};
        }
        if (socket_type == Type::STREAM && received_packets.empty()) {
            return {0, Errno::SUCCESS};
        }
    }
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::Receive(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    ASSERT((flags & ~(FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT)) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    std::unique_lock lock(packets_mutex);
    if (closed) {
        return {-1, Errno::BADF};
    }
    if (received_packets.empty()) {
        if (!blocking || (flags & FLAG_MSG_DONTWAIT) != 0) {
            return {-1, Errno::AGAIN};
        }
        // TODO (flTobi): Verify the timeout behavior and break when connection is lost
        // When receive_timeout is set to zero, the socket is supposed to wait indefinitely until a
        // packet arrives. In order to prevent lost packets from hanging the emulation thread, we set
        // the timeout to 5s instead. An expired wait is reported like an SO_RCVTIMEO timeout.
        const u32 indefinite_wait_cap_ms = SwitchSportsLanFixesEnabled() ? 250 : 5000;
        const u32 timeout = receive_timeout == 0 ? indefinite_wait_cap_ms : receive_timeout;
        if (!packets_cv.wait_for(lock, std::chrono::milliseconds{timeout},
                                 [this] { return closed || !received_packets.empty(); })) {
            return {-1, Errno::AGAIN};
        }
        if (closed) {
            return {-1, Errno::BADF};
        }
    }

    return ReceivePacket(flags, message, addr, message.size(), msg_flags);
}

std::pair<s32, Errno> ProxySocket::ReceivePacket(int flags, std::span<u8> message, SockAddrIn* addr,
                                                 std::size_t max_length, u32& msg_flags) {
    const ProxyPacket& packet = received_packets.front();
    if (addr) {
        addr->family = Domain::INET;
        addr->ip = packet.local_endpoint.ip;         // The senders ip address
        addr->portno = packet.local_endpoint.portno; // The senders port number
    }

    const std::span<const u8> unread = std::span{packet.data}.subspan(front_read_offset);
    const std::size_t read_bytes = (std::min)(unread.size(), max_length);
    std::memcpy(message.data(), unread.data(), read_bytes);

    // Unlike a stream's, the part of a datagram that does not fit is never read: report it.
    if (socket_type != Type::STREAM && read_bytes < unread.size()) {
        msg_flags |= FLAG_MSG_TRUNC;
    }

    if ((flags & FLAG_MSG_PEEK) != 0) {
        return {static_cast<s32>(read_bytes), Errno::SUCCESS};
    }

    if (socket_type == Type::STREAM && read_bytes < unread.size()) {
        // Keep the unread part of the stream for the next call
        front_read_offset += read_bytes;
    } else {
        // Like BSD recv(), the rest of a datagram that does not fit is discarded
        received_packets.pop();
        front_read_offset = 0;
    }

    return {static_cast<s32>(read_bytes), Errno::SUCCESS};
}

std::pair<s32, Errno> ProxySocket::Send(std::span<const u8> message, int flags) {
    std::lock_guard guard(packets_mutex);
    if (closed) {
        return {-1, Errno::BADF};
    }
    LOG_WARNING(Network, "(STUBBED) called");
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    return {static_cast<s32>(0), Errno::SUCCESS};
}

std::pair<s32, Errno> ProxySocket::SendTo(u32 flags, std::span<const u8> message,
                                          const SockAddrIn* addr) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);
    if (!addr)
        return {-1, Errno::INVAL};

    if (protocol == Protocol::ICMP && SwitchSportsLanFixesEnabled()) {
        constexpr u8 IcmpEchoRequest = 8;
        if (addr && message.size() >= 8 && message[0] == IcmpEchoRequest) {
            IPv4Address own_ip{127, 0, 0, 1};
            if (const auto room_ip = GetRoomIPv4Address()) {
                own_ip = *room_ip;
            } else if (const auto host_ip = GetHostIPv4Address()) {
                own_ip = *host_ip;
            }
            LOG_DEBUG(Network, "Answering ICMP echo to {}.{}.{}.{}", addr->ip[0], addr->ip[1],
                      addr->ip[2], addr->ip[3]);
            {
                std::lock_guard guard(packets_mutex);
                if (!closed) {
                    received_packets.push(ProxyPacket{
                        .local_endpoint = *addr,
                        .remote_endpoint = local_endpoint,
                        .protocol = protocol,
                        .broadcast = false,
                        .data = MakeIcmpEchoReply(message, addr->ip, own_ip),
                    });
                }
            }
            packets_cv.notify_all();
        }
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    if (SwitchSportsLanFixesEnabled()) {
        bool unbound;
        {
            std::lock_guard guard(packets_mutex);
            unbound = !is_bound && !closed;
        }
        if (unbound) {
            SockAddrIn ephemeral{};
            ephemeral.family = Domain::INET;
            Bind(ephemeral);
        }
    }

    ProxyPacket packet;
    bool bound, local_bind, datagram;
    {
        std::lock_guard guard(packets_mutex);
        if (closed) {
            return {-1, Errno::BADF};
        }
        if (addr->family != Domain::INET) {
            return {-1, Errno::AFNOSUPPORT};
        }
        bound = is_bound;
        local_bind = room_local_bind;
        datagram = socket_type == Type::DGRAM;
        packet.local_endpoint = local_endpoint;
        packet.protocol = protocol;
        packet.broadcast = broadcast && addr->ip[3] == 255;
    }

    if (!bound) {
        LOG_ERROR(Network, "ProxySocket is not bound!");
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    const auto room_member = Network::GetRoomMember().lock();
    if (room_member && !room_member->IsConnected()) {
        return {-1, Errno::CONNRESET};
    }

    if (datagram && addr->ip[0] != 127 && addr->ip[3] != 255 &&
        addr->ip != GetRoomIPv4Address() && SwitchSportsLanFixesEnabled()) {
        Service::Friend::NoteResortUnicastTarget(addr->ip);
    }

    packet.remote_endpoint = *addr;

    if (!local_bind || !room_member)
        return {-1, Errno::NOTCONN};
    const auto identity = room_member->GetENetIdentity();
    if (!identity)
        return {-1, Errno::CONNRESET};
    packet.local_endpoint.ip = identity->ip;
    packet.enet_generation = identity->generation;

    packet.data = Common::Compression::CompressDataZSTDDefault(message.data(), message.size());
    if (packet.remote_endpoint.ip[0] == 127 && SwitchSportsLanFixesEnabled()) {
        packet.local_endpoint.ip = packet.remote_endpoint.ip;
        room_member->DeliverLoopbackProxyPacket(packet);
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }
    if (!room_member->SendProxyPacket(packet, !datagram || !SwitchSportsLanFixesEnabled()))
        return {-1, Errno::CONNRESET};

    return {static_cast<s32>(message.size()), Errno::SUCCESS};
}

Errno ProxySocket::Close() {
    {
        std::lock_guard guard(packets_mutex);
        fd = INVALID_SOCKET;
        closed = true;
        if (is_bound)
            ReleaseBinding(this);
        received_packets = {};
        front_read_offset = 0;
    }
    packets_cv.notify_all();

    return Errno::SUCCESS;
}

Errno ProxySocket::SetLinger(bool enable, u32 linger) {
    struct Linger {
        u16 linger_enable;
        u16 linger_time;
    } values;
    values.linger_enable = enable ? 1 : 0;
    values.linger_time = static_cast<u16>(linger);

    return SetSockOpt(fd, SO_LINGER, values);
}

Errno ProxySocket::SetReuseAddr(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        reuse_addr = enable;
        if (is_bound) {
            auto& registry = GetBindings();
            std::lock_guard registry_guard(registry.mutex);
            for (auto& binding : registry.entries) {
                if (binding.owner == this) {
                    binding.reuse_addr = enable;
                }
            }
        }
    }
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno ProxySocket::SetReusePort(bool enable) {
    std::lock_guard guard(packets_mutex);
    reuse_port = enable;
    if (is_bound) {
        auto& registry = GetBindings();
        std::lock_guard registry_guard(registry.mutex);
        for (auto& binding : registry.entries) {
            if (binding.owner == this) {
                binding.reuse_port = enable;
            }
        }
    }
    return Errno::SUCCESS;
}

Errno ProxySocket::SetBroadcast(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        broadcast = enable;
    }
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

Errno ProxySocket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno ProxySocket::SetKeepAlive(bool enable) {
    return Errno::SUCCESS;
}

Errno ProxySocket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

Errno ProxySocket::SetSndTimeo(u32 value) {
    send_timeout = value;
    return SetSockOpt(fd, SO_SNDTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetRcvTimeo(u32 value) {
    {
        std::lock_guard guard(packets_mutex);
        receive_timeout = value;
    }
    return SetSockOpt(fd, SO_RCVTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetNoDelay(bool enable) {
    // Room sockets have no TCP stack; the option is only kept for GetNoDelay.
    std::lock_guard guard(packets_mutex);
    no_delay = enable;
    return Errno::SUCCESS;
}

Errno ProxySocket::SetNonBlock(bool enable) {
    std::lock_guard guard(packets_mutex);
    blocking = !enable;
    return Errno::SUCCESS;
}

Errno ProxySocket::SetIpv6Option(Ipv6Option option, s32 value) {
    // Rooms carry IPv4 alone, so a room socket is never an INET6 one.
    return Errno::INVAL;
}

std::pair<s32, Errno> ProxySocket::GetIpv6Option(Ipv6Option option) {
    return {0, Errno::INVAL};
}

std::pair<Errno, Errno> ProxySocket::GetPendingError() {
    std::lock_guard guard(packets_mutex);
    if (closed) {
        return {Errno::SUCCESS, Errno::BADF};
    }
    LOG_DEBUG(Network, "(STUBBED) called");
    return {Errno::SUCCESS, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetReuseAddr() {
    std::lock_guard guard(packets_mutex);
    return {reuse_addr, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetReusePort() {
    std::lock_guard guard(packets_mutex);
    return {reuse_port, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetNoDelay() {
    std::lock_guard guard(packets_mutex);
    return {no_delay, Errno::SUCCESS};
}

bool ProxySocket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

} // namespace Network
