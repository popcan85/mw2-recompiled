// Players on the same network, over plain UDP: the smallest real service, and
// the one that tests the kernel side without an account anywhere. Two copies on
// one machine find each other too, which the title's own sockets cannot do --
// both would want port 1000.
//
// Each copy sends from a port of its own, and a peer is that port's address:
// the IPv4 address in the top bits, the port in the low 16. Broadcasts go to a
// port every copy shares (MW2_LAN_PORT, 3074 by default), which the kernel
// delivers to all of them; each copy's own port is the first free one after it.
// Who the player is, to the title, is the profile at the first controller
// (signin.h), plus this copy's port slot, so a second copy on one machine is a
// second player. MW2_NAME is another name for him than the profile's.
//
// Xbox LIVE lobbies: every copy says who it is and what can be joined, once a
// second, to everyone on the shared port. MW2_LAN_JOIN=<name> joins that
// player's party or lobby as soon as it is seen, as choosing a friend and
// "Join Session" does on the console. The lobby's "Invite friends" invites
// every player seen; a copy started with MW2_LAN_ACCEPT=1 accepts.
#include "service.h"
#include "../env.h"
#include "../log.h"
#include "../signin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The few places the two socket APIs differ.
namespace
{
#ifdef _WIN32
    using Socket = SOCKET;
    const Socket kNoSocket = INVALID_SOCKET;
    void CloseSocket(Socket s) { closesocket(s); }
    void MakeNonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
    const char* UserName() { return std::getenv("USERNAME"); }
#else
    using Socket = int;
    const Socket kNoSocket = -1;
    void CloseSocket(Socket s) { ::close(s); }
    void MakeNonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
    const char* UserName() { return std::getenv("USER"); }
#endif

    bool SendDatagram(Socket s, const void* data, size_t size, const sockaddr_in& to)
    {
        return ::sendto(s, static_cast<const char*>(data), int(size), 0,
                        reinterpret_cast<const sockaddr*>(&to), sizeof to) >= 0;
    }

    // -1 when nothing is waiting.
    long ReceiveDatagram(Socket s, void* data, size_t size, sockaddr_in& from)
    {
        socklen_t length = sizeof from;
        return long(::recvfrom(s, static_cast<char*>(data), int(size), 0,
                               reinterpret_cast<sockaddr*>(&from), &length));
    }

    uint64_t Fnv(const std::string& text)
    {
        uint64_t hash = 0xCBF29CE484222325ull;   // FNV-1a
        for (char c : text) hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
        return hash;
    }

    // The account travels big-endian; every host this runs on is little-endian.
    uint64_t BigEndian64(uint64_t value) { return __builtin_bswap64(value); }
}

namespace
{
    // Magic, the sender's instance, its port and the port it is for.
    struct Frame
    {
        uint32_t magic;
        uint32_t instance;
        uint16_t fromPort, toPort;
    };
    constexpr uint32_t kMagic = 0x4D57324C;   // "MW2L"

    // Who a copy is and what can be joined: to everyone as presence, or to one
    // player as an invitation.
    struct Presence
    {
        uint32_t magic;
        uint32_t instance;
        uint8_t kind;          // kAnnounce or kInvite
        uint8_t joinable;      // `session` holds something
        uint8_t pad[2];
        uint64_t account;      // big-endian
        char name[32];
        uint8_t session[online::kSessionInfoSize];
    };
    constexpr uint32_t kPresenceMagic = 0x4D573250;   // "MW2P"
    constexpr uint8_t kAnnounce = 0, kInvite = 1;
    constexpr auto kAnnounceEvery = std::chrono::seconds(1);

    struct Peer
    {
        uint64_t id = 0, account = 0;
        bool joinable = false;
        uint8_t session[online::kSessionInfoSize]{};
    };
    constexpr size_t kLargest = 64 * 1024;
    constexpr size_t kQueued = 512;

    uint64_t IdOf(const sockaddr_in& address)
    {
        return (uint64_t(ntohl(address.sin_addr.s_addr)) << 16) | ntohs(address.sin_port);
    }

    sockaddr_in AddressOf(uint64_t id)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(uint32_t(id >> 16));
        address.sin_port = htons(uint16_t(id));
        return address;
    }

    uint32_t MaskOf(unsigned prefix)
    {
        return prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
    }

    // The network connections that are up, the loopback aside, in the order
    // the system lists them, each with its netmask, whether it has a default
    // gateway (the connection to the rest of the network has one, a virtual
    // machine's switch not) and whether it is a network card, Ethernet or
    // Wi-Fi, rather than a VPN's tunnel (which may have a gateway too).
    struct Adapter { std::string name; uint32_t address, mask; bool gateway, card; };
    std::vector<Adapter> Adapters()
    {
        std::vector<Adapter> found;
#ifdef _WIN32
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER
                               | GAA_FLAG_INCLUDE_GATEWAYS;
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        }
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) != NO_ERROR) return found;
        for (auto* at = adapters; at; at = at->Next)
        {
            if (at->OperStatus != IfOperStatusUp || at->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char name[128] = "?";
            if (at->FriendlyName)
                WideCharToMultiByte(CP_UTF8, 0, at->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
            name[sizeof name - 1] = 0;
            const bool gateway = at->FirstGatewayAddress != nullptr;
            const bool card = at->IfType == IF_TYPE_ETHERNET_CSMACD || at->IfType == IF_TYPE_IEEE80211;
            for (auto* unicast = at->FirstUnicastAddress; unicast; unicast = unicast->Next)
            {
                const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                if (address->sin_family == AF_INET)
                    found.push_back({ name, ntohl(address->sin_addr.s_addr),
                                      MaskOf(unicast->OnLinkPrefixLength), gateway, card });
            }
        }
#else
        // Linux keeps the gateways in its routing table: a default route's
        // destination and mask are both zero.
        std::vector<std::string> gateways;
        std::ifstream routes("/proc/net/route");
        std::string line;
        std::getline(routes, line);   // the heading
        while (std::getline(routes, line))
        {
            char name[64];
            unsigned destination = 1, mask = 1;
            if (std::sscanf(line.c_str(), "%63s %x %*x %*x %*d %*d %*d %x", name, &destination, &mask) == 3
                && destination == 0 && mask == 0)
                gateways.push_back(name);
        }
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) return found;
        for (ifaddrs* at = list; at; at = at->ifa_next)
        {
            if (!at->ifa_addr || at->ifa_addr->sa_family != AF_INET) continue;
            if (!(at->ifa_flags & IFF_UP) || (at->ifa_flags & IFF_LOOPBACK)) continue;
            const uint32_t mask = at->ifa_netmask
                ? ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_netmask)->sin_addr.s_addr) : 0;
            const bool gateway = std::find(gateways.begin(), gateways.end(), at->ifa_name) != gateways.end();
            // A network card has a device behind it; a tunnel or a bridge has none.
            const bool card = access(("/sys/class/net/" + std::string(at->ifa_name) + "/device").c_str(), F_OK) == 0;
            found.push_back({ at->ifa_name, ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_addr)->sin_addr.s_addr),
                              mask, gateway, card });
        }
        freeifaddrs(list);
#endif
        return found;
    }

    std::string Text(uint32_t address)
    {
        in_addr in;
        in.s_addr = htonl(address);
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &in, text, sizeof text);
        return text;
    }

    bool Private(uint32_t address)   // 10/8, 172.16/12, 192.168/16
    {
        return (address >> 24) == 10 || (address >> 20) == 0xAC1 || (address >> 16) == 0xC0A8;
    }

    bool LinkLocal(uint32_t address) { return (address >> 16) == 0xA9FE; }   // 169.254/16

    // How likely a connection is to be the one the other players are on, best
    // first: a private address with a gateway is the home or office network,
    // on a network card before a VPN's tunnel with a private address and a
    // gateway of its own (Mullvad's); a private one without is a virtual
    // machine's switch (Hyper-V's vEthernet); a public one, or a VPN's
    // (Tailscale's 100.64/10), comes after; a link-local address, which
    // Windows gives a connection that found no server to ask, reaches nobody
    // else.
    int Rank(const Adapter& adapter)
    {
        if (LinkLocal(adapter.address)) return 5;
        if (Private(adapter.address)) return (adapter.gateway ? 0 : 2) + (adapter.card ? 0 : 1);
        return 4;
    }

    // Where other copies reach this one, and where its broadcasts go.
    struct Own { uint32_t address, broadcast; };

    // The connection the other players are most likely on (Rank), the first
    // the system lists among equals, or the loopback when there is none.
    // MW2_LAN_ADDRESS=<address> names it; MW2_LAN_ADDRESS=first takes the
    // first listed, as before. Broadcasts go to the chosen network's own
    // broadcast address, which leaves by that connection: the system sends
    // 255.255.255.255 out of one connection of its choosing.
    Own OwnAddress()
    {
        const std::vector<Adapter> adapters = Adapters();
        if (adapters.empty())
        {
            LOGI("online: no network connection is up; lan stays on this machine");
            return { INADDR_LOOPBACK, INADDR_BROADCAST };
        }
        std::string list;
        for (const Adapter& adapter : adapters)
        {
            list += (list.empty() ? "" : ", ") + adapter.name + " " + Text(adapter.address) + " ("
                  + (LinkLocal(adapter.address) ? "link-local, " : Private(adapter.address) ? "private, " : "")
                  + (adapter.gateway ? "gateway, " : "") + (adapter.card ? "card)" : "virtual)");
        }
        LOGI("online: network connections: %s", list.c_str());

        const char* wanted = env::Text("MW2_LAN_ADDRESS");
        if (wanted && std::strcmp(wanted, "first") == 0)
        {
            const Adapter& first = adapters.front();
            LOGI("online: announcing the first, %s %s (MW2_LAN_ADDRESS=first)", first.name.c_str(),
                 Text(first.address).c_str());
            return { first.address, INADDR_BROADCAST };
        }
        const Adapter* chosen = nullptr;
        if (wanted)
        {
            in_addr parsed;
            if (inet_pton(AF_INET, wanted, &parsed) != 1)
                LOGW("online: MW2_LAN_ADDRESS=%s is not an IPv4 address; choosing one", wanted);
            else
            {
                const uint32_t address = ntohl(parsed.s_addr);
                for (const Adapter& adapter : adapters)
                    if (adapter.address == address) { chosen = &adapter; break; }
                if (!chosen)
                {
                    LOGW("online: no network connection has %s (MW2_LAN_ADDRESS); announcing it anyway", wanted);
                    return { address, INADDR_BROADCAST };
                }
            }
        }
        const bool named = chosen != nullptr;
        if (!chosen)
            chosen = &*std::min_element(adapters.begin(), adapters.end(),
                                        [](const Adapter& a, const Adapter& b) { return Rank(a) < Rank(b); });
        // A /31 or /32 has no broadcast address of its own.
        const uint32_t broadcast = chosen->mask && chosen->mask < 0xFFFFFFFEu
            ? chosen->address | ~chosen->mask : uint32_t(INADDR_BROADCAST);
        LOGI("online: announcing %s %s%s, broadcasts to %s", chosen->name.c_str(), Text(chosen->address).c_str(),
             named ? " (MW2_LAN_ADDRESS)" : "", Text(broadcast).c_str());
        if (LinkLocal(chosen->address))
            LOGW("online: %s is link-local: other machines are unlikely to reach it; MW2_LAN_ADDRESS names another",
                 Text(chosen->address).c_str());
        return { chosen->address, broadcast };
    }

    Socket OpenSocket(uint16_t port, bool shared)
    {
        Socket s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kNoSocket) return kNoSocket;
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
        if (shared) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        MakeNonBlocking(s);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        {
            CloseSocket(s);
            return kNoSocket;
        }
        return s;
    }

    class Lan final : public online::Service
    {
    public:
        ~Lan() override
        {
            if (own_ != kNoSocket) CloseSocket(own_);
            if (shared_ != kNoSocket) CloseSocket(shared_);
        }

        bool Start() override
        {
#ifdef _WIN32
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LOGE("online: no Winsock"); return false; }
#endif
            sharedPort_ = uint16_t(env::Number("MW2_LAN_PORT", 3074));
            // A port of its own, the first free one above the shared port.
            for (slot_ = 0; slot_ < 64 && own_ == kNoSocket; slot_++)
                own_ = OpenSocket(uint16_t(sharedPort_ + 1 + slot_), false);
            slot_--;
            shared_ = OpenSocket(sharedPort_, true);
            if (own_ == kNoSocket || shared_ == kNoSocket)
            {
                LOGE("online: lan could not open its sockets (shared port %u)", sharedPort_);
                return false;
            }
            sockaddr_in bound{};
            socklen_t length = sizeof bound;
            getsockname(own_, reinterpret_cast<sockaddr*>(&bound), &length);
            const Own own = OwnAddress();
            bound.sin_addr.s_addr = htonl(own.address);
            broadcast_ = own.broadcast;
            id_ = IdOf(bound);
            instance_ = std::random_device{}();
            const signin::Player player = signin::First();
            account_ = player.id;
            // Until there were profiles, the account was made from the name.
            const char* name = env::Text("MW2_NAME");
            const char* login = name ? name : UserName();
            former_ = Fnv(login ? login : "Player");
            name_ = name ? name : player.name;
            named_ = name != nullptr;
            if (const char* join = env::Text("MW2_LAN_JOIN")) join_ = join;
            accept_ = env::Flag("MW2_LAN_ACCEPT");
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &bound.sin_addr, text, sizeof text);
            LOGI("online: lan as \"%s\" at %s:%u, broadcasts on %u", name_.c_str(), text,
                 ntohs(bound.sin_port), sharedPort_);
            return true;
        }

        uint64_t LocalId() override { return id_; }
        // The name, and which copy on this machine this is: the first to start
        // takes the first slot, so a second copy is a second player.
        uint64_t Account() override { return account_ + slot_; }
        uint64_t FormerAccount() override { return former_ + slot_; }
        // MW2_NAME's, or the first controller's profile's as it is now: the
        // sign-in screen can name it again.
        std::string LocalName() override { return named_ ? name_ : signin::First().name; }

        bool Send(uint64_t peer, uint16_t fromPort, uint16_t toPort,
                  const void* bytes, size_t size) override
        {
            return SendTo(AddressOf(peer), fromPort, toPort, bytes, size);
        }

        bool Broadcast(uint16_t fromPort, uint16_t toPort, const void* bytes, size_t size) override
        {
            sockaddr_in everyone{};
            everyone.sin_family = AF_INET;
            everyone.sin_addr.s_addr = htonl(broadcast_);
            everyone.sin_port = htons(sharedPort_);
            return SendTo(everyone, fromPort, toPort, bytes, size);
        }

        bool Receive(uint16_t port, online::Datagram& out) override
        {
            std::lock_guard g(lock_);
            Drain(own_);
            Drain(shared_);
            auto it = waiting_.find(port);
            if (it == waiting_.end() || it->second.empty()) return false;
            out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }

        bool Lobbies() override { return true; }

        void SetJoinable(const online::Joinable* joinable) override
        {
            std::lock_guard g(lock_);
            hasJoinable_ = joinable != nullptr;
            if (joinable) joinable_ = *joinable;
            AnnounceLocked();
        }

        bool NextInvite(online::Invite& out) override
        {
            std::lock_guard g(lock_);
            Drain(own_);
            Drain(shared_);
            const auto now = std::chrono::steady_clock::now();
            if (now - announced_ >= kAnnounceEvery) AnnounceLocked();
            if (!join_.empty())
            {
                auto it = peers_.find(join_);
                if (it != peers_.end() && it->second.joinable)
                {
                    LOGI("online: lan joins %s", join_.c_str());
                    invites_.push_back(InviteFrom(it->second, false));
                    join_.clear();
                }
            }
            if (invites_.empty()) return false;
            out = invites_.front();
            invites_.pop_front();
            return true;
        }

        void InviteFriends() override
        {
            std::lock_guard g(lock_);
            if (!hasJoinable_) { LOGW("online: nothing to invite anyone to yet"); return; }
            const Presence invite = PresenceLocked(kInvite);
            for (const auto& [name, peer] : peers_)
            {
                LOGI("online: lan invites %s", name.c_str());
                const sockaddr_in to = AddressOf(peer.id);
                SendDatagram(own_, &invite, sizeof invite, to);
            }
        }

    private:
        static online::Invite InviteFrom(const Peer& peer, bool fromInvite)
        {
            online::Invite invite;
            invite.inviter = peer.account;
            std::memcpy(invite.session, peer.session, sizeof invite.session);
            invite.fromInvite = fromInvite;
            return invite;
        }

        Presence PresenceLocked(uint8_t kind)
        {
            Presence presence{};
            presence.magic = htonl(kPresenceMagic);
            presence.instance = instance_;
            presence.kind = kind;
            presence.joinable = hasJoinable_;
            presence.account = BigEndian64(Account());
            std::snprintf(presence.name, sizeof presence.name, "%s", LocalName().c_str());
            if (hasJoinable_) std::memcpy(presence.session, joinable_.session, sizeof presence.session);
            return presence;
        }

        void AnnounceLocked()
        {
            announced_ = std::chrono::steady_clock::now();
            const Presence presence = PresenceLocked(kAnnounce);
            sockaddr_in everyone{};
            everyone.sin_family = AF_INET;
            everyone.sin_addr.s_addr = htonl(broadcast_);
            everyone.sin_port = htons(sharedPort_);
            SendDatagram(own_, &presence, sizeof presence, everyone);
        }

        void Heard(const sockaddr_in& from, const Presence& presence)
        {
            if (presence.instance == instance_) return;
            const std::string name(presence.name, strnlen(presence.name, sizeof presence.name));
            Peer& peer = peers_[name];
            peer.id = IdOf(from);
            peer.account = BigEndian64(presence.account);
            if (peer.account == Account() && !warned_)
            {
                warned_ = true;
                LOGE("online: %s on this network is the same player as this one; on one of the two machines,"
                     " take the `machine` line out of saves/profiles.txt", name.c_str());
            }
            peer.joinable = presence.joinable;
            std::memcpy(peer.session, presence.session, sizeof peer.session);
            if (presence.kind != kInvite || !peer.joinable) return;
            if (accept_)
            {
                LOGI("online: lan accepts %s's invitation", name.c_str());
                invites_.push_back(InviteFrom(peer, true));
            }
            else
                LOGI("online: %s invites you (MW2_LAN_ACCEPT=1 accepts)", name.c_str());
        }

        bool SendTo(const sockaddr_in& to, uint16_t fromPort, uint16_t toPort,
                    const void* bytes, size_t size)
        {
            if (size > kLargest - sizeof(Frame)) return false;
            thread_local std::vector<uint8_t> packet;
            packet.resize(sizeof(Frame) + size);
            const Frame frame{ htonl(kMagic), instance_, htons(fromPort), htons(toPort) };
            std::memcpy(packet.data(), &frame, sizeof frame);
            std::memcpy(packet.data() + sizeof frame, bytes, size);
            return SendDatagram(own_, packet.data(), packet.size(), to);
        }

        // Everything the socket holds, sorted by the port it is for. A broadcast
        // comes back to the copy that sent it; the instance tells it apart.
        void Drain(Socket s)
        {
            thread_local std::vector<uint8_t> packet(kLargest);
            for (;;)
            {
                sockaddr_in from{};
                const long got = ReceiveDatagram(s, packet.data(), packet.size(), from);
                if (got < 0) return;
                uint32_t magic = 0;
                if (size_t(got) >= sizeof magic) std::memcpy(&magic, packet.data(), sizeof magic);
                if (ntohl(magic) == kPresenceMagic && size_t(got) == sizeof(Presence))
                {
                    Presence presence;
                    std::memcpy(&presence, packet.data(), sizeof presence);
                    Heard(from, presence);
                    continue;
                }
                Frame frame;
                if (size_t(got) < sizeof frame) continue;
                std::memcpy(&frame, packet.data(), sizeof frame);
                if (ntohl(frame.magic) != kMagic || frame.instance == instance_) continue;
                online::Datagram datagram;
                datagram.from = IdOf(from);
                datagram.fromPort = ntohs(frame.fromPort);
                datagram.toPort = ntohs(frame.toPort);
                datagram.bytes.assign(packet.begin() + sizeof frame, packet.begin() + got);
                // A port nobody reads fills up and drops, as a socket's buffer does.
                auto& queue = waiting_[datagram.toPort];
                if (queue.size() < kQueued) queue.push_back(std::move(datagram));
            }
        }

        Socket own_ = kNoSocket, shared_ = kNoSocket;
        uint16_t sharedPort_ = 0;
        uint32_t broadcast_ = INADDR_BROADCAST;   // where announcements and the title's broadcasts go
        uint64_t id_ = 0;
        uint32_t instance_ = 0;
        uint32_t slot_ = 0;
        std::string name_;
        bool named_ = false;   // by MW2_NAME
        uint64_t account_ = 0, former_ = 0;
        bool warned_ = false;                  // told once that another machine has this identity
        std::mutex lock_;
        std::map<uint16_t, std::deque<online::Datagram>> waiting_;

        std::map<std::string, Peer> peers_;   // by name
        bool hasJoinable_ = false;
        online::Joinable joinable_;
        std::chrono::steady_clock::time_point announced_{};
        std::string join_;                     // MW2_LAN_JOIN, until joined
        bool accept_ = false;                  // MW2_LAN_ACCEPT
        std::deque<online::Invite> invites_;
    };
}

std::unique_ptr<online::Service> online::MakeLan() { return std::make_unique<Lan>(); }
