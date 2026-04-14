/**
 * @file Common.h
 * @brief Shared definitions for the Aircraft Telemetry Client/Server system.
 *
 * Defines the network packet structure, port constants, and utility
 * functions used by both the Client and Server applications.
 *
 * Course:  CSCN73060 - Client/Server Project
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <string>

// ─── Network Configuration ────────────────────────────────────────────────────
#define DEFAULT_SERVER_IP   "127.0.0.1"   ///< Default server IP (override via argv)
#define SERVER_PORT         55555          ///< TCP port the server listens on
#define BACKLOG             128            ///< Max pending connections in listen queue

// ─── Packet Layout ────────────────────────────────────────────────────────────
#define TIMESTAMP_LEN       32             ///< Fixed buffer for "D_M_YYYY HH:MM:SS\0"

/**
 * @brief Wire-format packet transmitted from client to server.
 *
 * Every telemetry line from the data file is packed into one of these
 * before being sent over TCP.  The @p isEOF flag lets the server know
 * the client has finished its telemetry file so it can finalise the
 * average fuel-consumption record for that airplane.
 */
#pragma pack(push, 1)
struct TelemetryPacket
{
    uint32_t clientID;              ///< Unique aircraft ID assigned at client startup
    char     timestamp[TIMESTAMP_LEN]; ///< Null-terminated timestamp string
    float    fuelRemaining;         ///< Gallons of fuel remaining at this sample
    uint8_t  isEOF;                 ///< 1 = last packet for this flight, 0 = normal data
};
#pragma pack(pop)

// ─── Timestamp Parsing ────────────────────────────────────────────────────────

/**
 * @brief Convert a telemetry timestamp string to a POSIX time_t.
 *
 * Handles the format produced by the data files:
 *   "D_M_YYYY HH:MM:SS"  (e.g. "12_3_2023 14:56:47")
 *
 * @param ts  Null-terminated timestamp string.
 * @return    Corresponding time_t, or (time_t)-1 on parse failure.
 */
inline time_t parseTimestamp(const char* ts)
{
    int p1 = 0, p2 = 0, year = 0, hour = 0, min = 0, sec = 0;
    if (std::sscanf(ts, "%d_%d_%d %d:%d:%d",
                    &p1, &p2, &year, &hour, &min, &sec) != 6)
    {
        return (time_t)-1;
    }

    // The data files use D_M_YYYY ordering; treat p1=day, p2=month.
    struct tm t = {};
    t.tm_mday  = p1;
    t.tm_mon   = p2 - 1;          // tm_mon is 0-based
    t.tm_year  = year - 1900;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    t.tm_isdst = -1;              // let the C runtime figure out DST
    return std::mktime(&t);
}
