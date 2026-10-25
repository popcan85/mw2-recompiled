// iw4x/Quake 3: a client takes packets about its connection only from the
// server it is connected to -- drop the ones from anyone else.
//
// CL_PacketEvent (sub_82189A88) is the client's entry for every packet that
// reaches a local client. A connectionless packet (first word -1) goes on to
// the out-of-band dispatcher; any other is a sequenced packet of the game
// connection, and once the connection state is at least 5 it goes straight to
// Netchan_Process (sub_8228E010) and from there to CL_ParseServerMessage. No
// step compares the sender with the netchan's remote address: CL_PacketEvent
// passes Netchan_Process only the channel and the message, and Netchan_Process
// tests only that the packet's own sequence number is newer than the last
// (cmpw r26,r29; bgt). Its callers (the queue drain sub_8227FFA8 and the
// local-client loop sub_82280028) do not compare either. Quake 3 and iw4x
// drop the packet unless NET_CompareAdr(from, netchan.remoteAddress) holds.
// Without the check, any peer the transport lets through (on lan, anyone on
// the network; on steam, any lobby member) can hand a connected client a whole
// server message -- snapshots, and reliable server commands such as the
// set-dvar command, which writes any dvar past its flags -- and one packet
// with sequence 0x7FFFFFFF makes the client drop every later packet from the
// real server as out of order, so it times out.
//
// The out-of-band connection handshake has the same gap. In the dispatcher
// (sub_82188DD0) challengeResponse, connectResponse and statResponse test only
// the connection state: challengeResponse (state 3) then stores the sender as
// the server address, so the connect line, with the player's userinfo, goes to
// whoever answered; connectResponse (state 4) sets the netchan up to the sender,
// which the check above would then accept; statResponse (state 6) moves the
// stat sync on. Quake 3 and iw4x refuse challengeResponse and connectResponse
// from an address other than clc.serverAddress. The dispatcher's other
// connection-changing commands already check: error, loadingnewmap and
// fastrestart compare the IP, and disconnect the netchan's whole address.
//
// So do the in-game host migration messages, which the migration dispatcher
// (sub_8218BDF8, which CL_ConnectionlessPacket calls among its commands) takes
// from anyone. The server streams its game state to the client it picked as
// the next host: mstart starts it, mhead brings the save header, mdata the
// state in 1200-byte chunks, each with a checksum of its own bytes only, and
// mstate the server's progress. The client loads what it got when it becomes
// the host (G_InitGame sub_82233A68 -> sub_8223F360). The server sends all four
// from its own socket (NET_OutOfBandData sub_8228EF80 with NS_SERVER, e.g.
// sub_822C54A0), and the client acknowledges mstart, mhead and mdata to its
// server address (sub_8218B750), never to the sender. Taking them only from that
// address is the rule the acknowledgements already assume. ping and pinga stay
// open: members ping each other, and the ping answer goes only to a member.
//
// The rule is the console's. Its secure network layer gives each peer one
// address, and the server sends everything from the socket the client reached
// it on, so the checks never refuse the real server. It holds here too:
// XNetXnAddrToInAddr and recvfrom (runtime/kernel/net.cpp) give a peer the same
// stand-in address, both keyed by the service's peer id, and the loopback
// address of a copy's own server compares equal by type. The one setup where
// a real server's packets would carry another address is a lan host whose
// replies leave by a different interface than the first adapter it announces
// (lan.cpp OwnAddress); the party code's XNADDR lookups already fail there.
//
// The compare is the title's own NET_CompareAdr (sub_8228E5B0, the one the
// disconnect handler uses): same type, then for an IP address the four bytes
// and the port; two addresses of any other type (the loopback) compare equal
// on the type alone, which the transport sets, not the sender. A refused packet gets the answer the original gives a packet
// it does not take (0, "not for this local client"), which sends the
// local-client loop on to the next client as it does for a packet that arrives
// before the connection is up. The one difference: a runt packet (under four
// bytes) from someone else now gets that 0 too, where the original prints
// "Runt packet" and answers 1, which only lets the loop offer it to the other
// local clients as well. MP-only: the campaign's packet handler
// (sub_8233C508) looks up the co-op client by the sender's address and answers
// anyone else with coopDis.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "log.h"
#include "guest.h"

#include "split_live.h"

#include <atomic>
#include <cctype>
#include <cstdint>

#if defined(T_CL_PacketEvent) && defined(T_CL_ConnectionlessPacket) && defined(T_NET_CompareAdr)

namespace {

// Per local client: the connection state, and the client connection, which
// holds the server address and the game netchan.
uint32_t ConnectionState(uint8_t* base, uint32_t localClient)
{
    return PPC_LOAD_U32(T_DATA_ClientConnState + localClient * T_CONN_STATE_STRIDE);
}

uint32_t Connection(uint8_t* base, uint32_t localClient)
{
    return PPC_LOAD_U32(T_DATA_ClientConnections) + localClient * T_CONNECTION_STRIDE;
}

// A netadr_t (type, four IP bytes, port) arrives in two registers: bytes 0..7,
// then bytes 8..11 in the high word of the second, which is also how
// NET_CompareAdr takes it.
bool SameAddress(PPCContext& ctx, uint8_t* base, uint64_t from0, uint64_t from1, uint32_t adr)
{
    const PPCContext saved = ctx;
    ctx.r3.u64 = from0;
    ctx.r4.u64 = from1;
    ctx.r5.u64 = PPC_LOAD_U64(adr);
    ctx.r6.u64 = uint64_t(PPC_LOAD_U32(adr + 8)) << 32;
    GUEST_FUNC(T_NET_CompareAdr)(ctx, base);
    const bool same = ctx.r3.u32 != 0;
    ctx = saved;
    return same;
}

// The sender as the log shows it: type, then IP:port for an IP address.
void Refused(const char* what, uint64_t from0, uint64_t from1, std::atomic<uint32_t>& count)
{
    // Every power of two, so a flood stays a few lines.
    const uint32_t n = ++count;
    if (n & (n - 1))
        return;
    const uint32_t ip = uint32_t(from0);
    LOGW("online: dropped %s from %u.%u.%u.%u:%u (type %u), not the server (%u so far)", what,
         ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, unsigned(from1 >> 48),
         unsigned(from0 >> 32), n);
}

std::atomic<uint32_t> g_sequenced, g_challenge, g_connect, g_stat, g_migration;

} // namespace

// CL_PacketEvent(localClient = r3, from = r4:r5, msg = r6, time = r7) -> handled
GUEST_HOOK(T_CL_PacketEvent)
{
    const uint32_t localClient = ctx.r3.u32;
    const uint32_t msg = ctx.r6.u32;
    // msg->cursize at +20, msg->data at +8, as the original reads them.
    const bool connectionless = PPC_LOAD_U32(msg + 20) >= 4
                             && PPC_LOAD_U32(PPC_LOAD_U32(msg + 8)) == 0xFFFFFFFFu;
    // At a state below 5 the original refuses a sequenced packet itself.
    if (!connectionless && ConnectionState(base, localClient) >= 5
        && !SameAddress(ctx, base, ctx.r4.u64, ctx.r5.u64,
                        Connection(base, localClient) + T_CONNECTION_NETCHAN_ADR)) {
        Refused("a game packet", ctx.r4.u64, ctx.r5.u64, g_sequenced);
        ctx.r3.u64 = 0;
        return;
    }
    // MW2_SPLIT_LIVE: a packet that arrived at another local client's port.
    if (splitlive::PacketForAnother(localClient, uint32_t(ctx.r4.u64 >> 32))) {
        ctx.r3.u64 = 0;
        return;
    }
    GUEST_ORIG(T_CL_PacketEvent)(ctx, base);
}

namespace {

// The dispatcher's compare, I_stricmp (sub_82315b70, strnicmp with no length
// limit): ASCII case-folded.
bool SameName(const char* a, const char* b)
{
    for (size_t i = 0;; ++i) {
        const unsigned char x = static_cast<unsigned char>(a[i]);
        const unsigned char y = static_cast<unsigned char>(b[i]);
        if (std::tolower(x) != std::tolower(y))
            return false;
        if (!x)
            return true;
    }
}

} // namespace

// CL_ConnectionlessPacket(localClient = r3, from = r4:r5, msg = r6, time = r7)
// -> handled. Its wrapper (sub_82189898) has tokenized the line already.
GUEST_HOOK(T_CL_ConnectionlessPacket)
{
    const uint32_t localClient = ctx.r3.u32;
    const uint32_t args = T_DATA_CmdArgs;
    const uint32_t nest = PPC_LOAD_U32(args);
    const char* name = "";
    if (int32_t(PPC_LOAD_U32(args + 68 + nest * 4)) > 0)   // the original's argc test (ble -> "")
        if (const char* p = GuestPtr<const char>(PPC_LOAD_U32(PPC_LOAD_U32(args + 100 + nest * 4))))
            name = p;

    // Each command only at the one state where the original acts on it; at any
    // other it refuses the command itself.
    const uint32_t state = ConnectionState(base, localClient);
    std::atomic<uint32_t>* count = nullptr;
    if (state == 3 && SameName(name, "challengeResponse"))
        count = &g_challenge;
    else if (state == 4 && SameName(name, "connectResponse"))
        count = &g_connect;
    else if (state == 6 && SameName(name, "statResponse"))
        count = &g_stat;
    // The migration messages at any state: each handler tests its own stage,
    // and only the server has a reason to send one.
    else if (SameName(name, "mstart") || SameName(name, "mhead") || SameName(name, "mdata")
             || SameName(name, "mstate"))
        count = &g_migration;
    if (count && !SameAddress(ctx, base, ctx.r4.u64, ctx.r5.u64,
                              Connection(base, localClient) + T_CONNECTION_SERVER_ADR)) {
        Refused(name, ctx.r4.u64, ctx.r5.u64, *count);
        ctx.r3.u64 = 0;
        return;
    }
    // MW2_SPLIT_LIVE: another local client's challenge, on to the next.
    if (splitlive::ForAnotherClient(ctx, base, localClient)) {
        ctx.r3.u64 = 0;
        return;
    }
    GUEST_ORIG(T_CL_ConnectionlessPacket)(ctx, base);
}

#endif
