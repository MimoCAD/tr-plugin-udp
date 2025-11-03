// Trunk-Recorder Status Over UDP Plugin
// ********************************
// Requires trunk-recorder 5.0 or later.
// ********************************

#include <time.h>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <string>
#include <map>
#include <cstring>
#include <regex>
#include <mqtt/client.h>
#include <trunk-recorder/source.h>
#include <json.hpp>
#include <trunk-recorder/plugin_manager/plugin_api.h>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>    //time_formatters.hpp>
#include <boost/dll/alias.hpp>                          // for BOOST_DLL_ALIAS
#include <boost/property_tree/json_parser.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/crc.hpp>

// UDP Socket Includes.
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>      // getaddrinfo, freeaddrinfo
#include <arpa/inet.h>
#include <unistd.h>     // close
#include <errno.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
typedef int SOCKET;
struct UdpTarget {
    SOCKET sock;
    sockaddr_storage addr;
    socklen_t addrlen;
};

// Helper Consts
const int PLUGIN_SUCCESS = 0;
const int PLUGIN_FAILURE = 1;

using namespace std;
namespace logging = boost::log;

// Alais C++ types to Rust types.
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

enum Type : u8 {
    Type_Invalid = 0,
    Unit_On = 1,
    Unit_Off = 2,
    Unit_AckResp = 3,
    Unit_Join = 4,
    Unit_Data = 5,
    Unit_AnsReq = 6,
    Unit_Location = 7,
    Unit_PTTP = 8, // Push to Talk Pressed
};
static_assert(std::is_same_v<std::underlying_type_t<Type>, u8>, "Type must be u8");

// Packets
#pragma pack(push, 1)
struct Packet {
    // Header: 4 Bytes (32 bits)
    char hdr[2] = {'M', 'C'};       // Prefix: 'M','C'
    Type typ = Type::Type_Invalid;  // Type: 1 byte
    u8   len = 5;                   // Size: Whole packet size, including Header, System, Radio, Payload. Size = Len * 4;

    // System: 4 Bytes (32 bits)
    u32  p25Id = 0;     // [31:20] = SystemID (12b), [19:0] = WACN (20b)

    // Radio: 8 Bytes (64 bits)
    u16  nac = 0;       // NAC
    u16  tgId = 0;      // Talk Group ID
    u32  radioId = 0;   // Radio's Src ID

    // Payload: 4 Bytes (32 Bits)
    u32  ts = 0;        // Time Stamp (UNIX Epoch Seconds)

    bool operator==(const Packet& o) const {
        return
            typ == o.typ &&
            len == o.len &&
            p25Id == o.p25Id &&
            nac == o.nac &&
            tgId == o.tgId &&
            radioId == o.radioId &&
            ts == o.ts;
    }
};
#pragma pack(pop)

static_assert(sizeof(Packet) == 20, "Packet header must be 20 bytes");
static_assert(alignof(Packet) == 1, "Packet must be packed");

// Packet Helpers
inline constexpr u16 p25_system_id(u32 p) {
    return static_cast<u16>(p >> 20);
}
inline constexpr u32 p25_wacn(u32 p) {
    return p & 0xFFFFFu;
}
inline constexpr u16 p25_nac (u32 p) {
    return p & 0x0FFFu;
}
inline constexpr std::size_t payload_bytes(const Packet& p) {
    return static_cast<std::size_t>(p.len) * 4;
}
inline constexpr bool valid_hdr(const Packet& p) {
    return p.hdr[0] == 'M' && p.hdr[1] == 'C';
}
constexpr u32 make_p25id(u16 sysId, u32 wacn) {
    return (u32(sysId & 0x0FFF) << 20) | (wacn & 0xFFFFF);
}

class Status_Udp : public Plugin_Api
{
    // Trunk-Recorder
    Config *tr_config;
    std::vector<Source *> tr_sources;
    std::vector<System *> tr_systems;

    // Plugin Settings
    std::string log_prefix = "\t[Status UDP]\t";
    std::string udp_dest;
    bool unit_enabled = true;

    // Plugin Socket
    UdpTarget udp_socket = UdpTarget {};
    // Make sure we don't send the same packet muliple times.
    Packet last_packet = Packet{};

public:
    Status_Udp(){};

    // ********************************
    // trunk-recorder messages
    // ********************************

    // call_start()
    //   Send information about a new call or the unit initiating it.
    //   TRUNK-RECORDER PLUGIN API: Called when a call starts
    int call_start(Call *call) override
    {
        if (unit_enabled)
        {
            System* sys = call->get_system();
            boost::property_tree::ptree stat_node = call->get_stats();
            u32 source_id = stat_node.get<u32>("srcId");

            Packet pkt{};
            pkt.typ     = Type::Unit_PTTP;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.tgId    = call->get_talkgroup();
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        };

        return PLUGIN_SUCCESS;
    }

    // unit_registration()
    //   Unit registration on a system (on)
    //   TRUNK-RECORDER PLUGIN API: Called each REGISTRATION message
    int unit_registration(System *sys, long source_id) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_On;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_deregistration()
    //   Unit de-registration on a system (off)
    //   TRUNK-RECORDER PLUGIN API: Called each DEREGISTRATION message
    int unit_deregistration(System *sys, long source_id) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_Off;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_acknowledge_response()
    //   Unit acknowledge response (ackresp)
    //   TRUNK-RECORDER PLUGIN API: Called each ACKNOWLEDGE message
    int unit_acknowledge_response(System *sys, long source_id) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_AckResp;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_group_affiliation()
    //   Unit talkgroup affiliation (join)
    //   TRUNK-RECORDER PLUGIN API: Called each AFFILIATION message
    int unit_group_affiliation(System *sys, long source_id, long talkgroup_num) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_Join;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.tgId    = talkgroup_num;
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_data_grant()
    //   Unit data grant (data)
    //   TRUNK-RECORDER PLUGIN API: Called each DATA_GRANT message
    int unit_data_grant(System *sys, long source_id) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_Data;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_answer_request()
    //   TRUNK-RECORDER PLUGIN API: Called each UU_ANS_REQ message
    int unit_answer_request(System *sys, long source_id, long talkgroup_num) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_AnsReq;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.tgId    = talkgroup_num;
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // unit_location()
    //   Unit location/roaming update (location)
    //   TRUNK-RECORDER PLUGIN API: Called each LOCATION message
    int unit_location(System *sys, long source_id, long talkgroup_num) override
    {
        if (unit_enabled)
        {
            Packet pkt{};
            pkt.typ     = Type::Unit_Location;
            pkt.p25Id   = make_p25id(sys->get_sys_site_id(), sys->get_wacn());
            pkt.nac     = p25_nac(sys->get_nac());
            pkt.tgId    = talkgroup_num;
            pkt.radioId = source_id;
            pkt.ts      = time(NULL);

            return send_packet(pkt);
        }

        return PLUGIN_SUCCESS;
    }

    // ********************************
    // trunk-recorder plugin API & startup
    // ********************************

    // parse_config()
    //   TRUNK-RECORDER PLUGIN API: Called before init(); parses the config information for this plugin.
    int parse_config(json config_data) override
    {
        // Get values from this plugin's config.json section and load into class variables.
        udp_dest = config_data.value("destination", "udp://127.0.0.1:7767");

        // Print plugin startup info
        BOOST_LOG_TRIVIAL(info) << log_prefix << "destination:            " << udp_dest << endl;

        return PLUGIN_SUCCESS;
    }

    // init()
    //   TRUNK-RECORDER PLUGIN API: Plugin initialization; called after parse_config().
    int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) override
    {
        // Establish pointers to systems, sources, and configs if needed later.
        tr_sources = sources;
        tr_systems = systems;
        tr_config = config;

        return PLUGIN_SUCCESS;
    }

    // start()
    //   TRUNK-RECORDER PLUGIN API: Called after trunk-recorder finishes setup and the plugin is initialized
    int start() override
    {
        // Start the UDP connection
        open_udp_connection();

        return PLUGIN_SUCCESS;
    }

    // stop()
    int stop() override
    {
        // In the event that we choose to "connect" to a UDP socket,
        // We would then remove that connection in the stop function.
        return PLUGIN_SUCCESS;
    }

    // ********************************
    // Service Functions
    // ********************************

    // send_packet()
    // Send a UDP packet to the desingated host.
    int send_packet(Packet packet)
    {
        if (udp_socket.sock == INVALID_SOCKET || udp_socket.addrlen == 0) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "UDP socket not initialized";

            return PLUGIN_FAILED;
        }

        // Don't send duplicate packets.
        if (last_packet == packet) {
            return PLUGIN_SUCCESS;
        }

        std::vector<u8> data;
        data.resize(sizeof(Packet));
        std::memcpy(data.data(), &packet, sizeof(packet));

        ssize_t bytesSent = ::sendto(
            udp_socket.sock,
            data.data(),
            data.size(),
            0,
            reinterpret_cast<const sockaddr*>(&udp_socket.addr),
            udp_socket.addrlen
        );

        // Update the last packet, to this packet.
        last_packet = packet;

        if (bytesSent == -1) {
            int err = errno;
            BOOST_LOG_TRIVIAL(error) << log_prefix << "sendto failed (" << err << "): " << std::strerror(err);

            return PLUGIN_FAILURE;
        }

        return PLUGIN_SUCCESS;
    }

    void open_udp_connection()
    {
        this->udp_socket = make_udp_target(udp_dest);
        if (this->udp_socket.sock == INVALID_SOCKET) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "Failed to open UDP target for " << udp_dest;
        }
    }

    // Parse udp://host[:port], with default port 7727
    bool parse_udp_uri(const std::string& uri, std::string& host, std::string& port) {
        const std::string prefix = "udp://";
        if (uri.rfind(prefix, 0) != 0) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "Destination URI must start with udp://" << endl;

            return false;
        }

        auto without_scheme = uri.substr(prefix.size());
        auto colon = without_scheme.find_last_of(':');

        if (colon == std::string::npos) {
            host = without_scheme;
            port = "7767";  // default
        } else {
            host = without_scheme.substr(0, colon);
            port = without_scheme.substr(colon + 1);
            if (port.empty()) port = "7767"; // handle udp://host:
        }

        BOOST_LOG_TRIVIAL(info) << log_prefix << "parse_udp_uri: host: '" << host << "' port: '" << port << "'" << endl;

        return !host.empty();
    }

    UdpTarget make_udp_target(const std::string& uri) {
        UdpTarget target{};
        target.sock = INVALID_SOCKET;

        std::string host, port;
        if (!parse_udp_uri(uri, host, port)) {
            BOOST_LOG_TRIVIAL(error) << "Invalid URI format";
            return target;
        }

        if (host == "0.0.0.0" || host == "::") {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "Refusing to use unspecified address (" << host << ") as a destination";
            return target;
        }

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags    = AI_NUMERICSERV;

        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (rc != 0) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "getaddrinfo failed for " << host << ":" << port
                                     << " (" << gai_strerror(rc) << ")";
            return target;
        }

        target.sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (target.sock == INVALID_SOCKET) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "socket() failed";
            ::freeaddrinfo(res);
            return target;
        }

        std::memcpy(&target.addr, res->ai_addr, res->ai_addrlen);
        target.addrlen = static_cast<socklen_t>(res->ai_addrlen);

        // Optional: enable broadcast if you're targeting a broadcast address
        if (res->ai_family == AF_INET) {
            auto sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
            if (sin->sin_addr.s_addr == INADDR_BROADCAST) {
                int yes = 1;
                ::setsockopt(target.sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            }
        }

        ::freeaddrinfo(res);
        return target;
    }

    // ********************************
    // Create the plugin
    // ********************************

    // Factory method
    static boost::shared_ptr<Status_Udp> create()
    {
        return boost::shared_ptr<Status_Udp>(new Status_Udp());
    }
};

BOOST_DLL_ALIAS(
    Status_Udp::create, // <-- this function is exported with...
    create_plugin       // <-- ... this alias name
)
