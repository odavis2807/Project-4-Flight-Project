/**
 * @file Server.cpp
 * @brief Multi-threaded aircraft telemetry ground-station server.
 *
 * System Requirements satisfied:
 *   SYS-001  Unlimited client connections via one thread per client.
 *   SYS-010  Per-connection: read transmitted data, parse time + fuel,
 *            calculate and store running fuel-consumption.
 *   SYS-020  On flight-end store the final average fuel-consumption.
 *   SYS-030  Aircraft identified by the unique ID embedded in each packet.
 *
 * Build (MSVC):
 *   cl /EHsc /std:c++17 Server.cpp /link Ws2_32.lib
 *
 * Course: CSCN73060 - Client/Server Project
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <atomic>

#include "Common.h"

 // ─── Flight Record ─────────────────────────────────────────────────────────────

 /**
  * @brief Holds all in-flight and historical data for one aircraft.
  *
  * Kept in the global fleet map and updated by the thread that owns the
  * current TCP connection for that aircraft.
  */
struct FlightRecord
{
    uint32_t    aircraftID = 0;
    int         flightNumber = 0;      ///< Increments each new connection

    // ── current flight state ──────────────────────────────────────────────────
    bool        firstSample = true;
    time_t      prevTime = 0;
    float       prevFuel = 0.0f;
    double      totalConsumed = 0.0;    ///< Gallons burned this flight
    double      totalElapsed = 0.0;    ///< Seconds elapsed this flight
    uint64_t    sampleCount = 0;

    // ── historical averages (across all flights) ──────────────────────────────
    double      lifetimeConsumed = 0.0;  ///< Gallons across all flights
    double      lifetimeElapsed = 0.0;  ///< Seconds across all flights
    uint32_t    totalFlights = 0;
};

// ─── Global State ──────────────────────────────────────────────────────────────
static std::mutex              g_fleetMutex;
static std::map<uint32_t, FlightRecord> g_fleet;  ///< AircraftID -> record
static std::atomic<bool>       g_running(true);

// ─── Logging helpers ───────────────────────────────────────────────────────────

/// Thread-safe console + optional log-file output.
static std::mutex  g_logMutex;
static std::ofstream g_logFile;

static void log(const std::string& msg)
{
    std::time_t now = std::time(nullptr);
    char tbuf[20];
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", std::localtime(&now));

    std::lock_guard<std::mutex> lk(g_logMutex);
    std::cout << "[" << tbuf << "] " << msg << "\n";
    if (g_logFile.is_open())
        g_logFile << "[" << tbuf << "] " << msg << "\n";
}

// ─── Fleet Report ──────────────────────────────────────────────────────────────

/**
 * @brief Append the final average consumption for one aircraft to the
 *        fleet-wide summary CSV file.
 *
 * CSV columns: AircraftID, FlightNumber, AvgConsumption_GPH,
 *              LifetimeAvgConsumption_GPH
 *
 * @param rec  Completed (or just-updated) FlightRecord.
 */
static void saveFinalAverage(const FlightRecord& rec)
{
    static std::mutex csvMutex;
    std::lock_guard<std::mutex> lk(csvMutex);

    std::ofstream csv("fleet_averages.csv",
        std::ios::app | std::ios::out);
    if (!csv.is_open()) { return; }

    // Write header if the file was empty / newly created.
    csv.seekp(0, std::ios::end);
    if (csv.tellp() == 0)
    {
        csv << "AircraftID,FlightNumber,Flight_AvgGPH,"
            "Lifetime_AvgGPH,TotalFlights\n";
    }

    double flightGPH = (rec.totalElapsed > 0.0)
        ? (rec.totalConsumed / rec.totalElapsed) * 3600.0
        : 0.0;
    double lifetimeGPH = (rec.lifetimeElapsed > 0.0)
        ? (rec.lifetimeConsumed / rec.lifetimeElapsed) * 3600.0
        : 0.0;

    csv << std::fixed << std::setprecision(4)
        << rec.aircraftID << ","
        << rec.flightNumber << ","
        << flightGPH << ","
        << lifetimeGPH << ","
        << rec.totalFlights << "\n";
}

// ─── Per-Client Thread ─────────────────────────────────────────────────────────

/**
 * @brief Entry point for the thread that services a single client socket.
 *
 * Implements SYS-010 and SYS-030.  When the client signals EOF (or the
 * connection drops) SYS-020 is also satisfied by storing the final average.
 *
 * @param clientSocket  The connected socket returned by accept().
 */
static void clientThread(SOCKET clientSocket)
{
    // Enable TCP keep-alive so we detect abruptly-crashed clients.
    BOOL keepAlive = TRUE;
    setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE,
        reinterpret_cast<char*>(&keepAlive), sizeof(keepAlive));

    TelemetryPacket pkt;
    int             bytesExpected = static_cast<int>(sizeof(TelemetryPacket));
    bool            identified = false;
    uint32_t        aircraftID = 0;

    while (g_running.load())
    {
        // ── Receive exactly one packet (loop handles partial TCP reads) ───────
        int  totalReceived = 0;
        char* buf = reinterpret_cast<char*>(&pkt);

        while (totalReceived < bytesExpected)
        {
            int n = recv(clientSocket,
                buf + totalReceived,
                bytesExpected - totalReceived,
                0);
            if (n <= 0)
            {
                // Connection closed or error (crash test scenario)
                goto done;
            }
            totalReceived += n;
        }

        // ── SYS-030: identify the aircraft ───────────────────────────────────
        aircraftID = pkt.clientID;
        identified = true;

        // ── SYS-010: parse time + fuel, update consumption ───────────────────
        time_t currTime = parseTimestamp(pkt.timestamp);
        float  currFuel = pkt.fuelRemaining;

        {
            std::lock_guard<std::mutex> lk(g_fleetMutex);
            FlightRecord& rec = g_fleet[aircraftID];
            rec.aircraftID = aircraftID;

            if (rec.firstSample)
            {
                // New flight starting
                rec.flightNumber++;
                rec.firstSample = false;
                rec.prevTime = currTime;
                rec.prevFuel = currFuel;
                rec.totalConsumed = 0.0;
                rec.totalElapsed = 0.0;
                rec.sampleCount = 0;

                std::ostringstream oss;
                oss << "Aircraft " << aircraftID
                    << " - Flight #" << rec.flightNumber
                    << " started. Initial fuel: "
                    << std::fixed << std::setprecision(2) << currFuel << " gal";
                log(oss.str());
            }
            else if (currTime != (time_t)-1 && rec.prevTime != (time_t)-1)
            {
                double dt = std::difftime(currTime, rec.prevTime);  // seconds
                if (dt > 0.0)
                {
                    double consumed = static_cast<double>(rec.prevFuel - currFuel);
                    if (consumed > 0.0)   // guard against sensor noise
                    {
                        rec.totalConsumed += consumed;
                        rec.totalElapsed += dt;
                        rec.sampleCount++;
                    }
                }
                rec.prevTime = currTime;
                rec.prevFuel = currFuel;
            }
        }

        // ── SYS-010c: print running average to console every 60 samples ──────
        {
            std::lock_guard<std::mutex> lk(g_fleetMutex);
            FlightRecord& rec = g_fleet[aircraftID];
            if (rec.sampleCount > 0 && rec.sampleCount % 60 == 0)
            {
                double gph = (rec.totalElapsed > 0.0)
                    ? (rec.totalConsumed / rec.totalElapsed) * 3600.0
                    : 0.0;
                std::ostringstream oss;
                oss << "Aircraft " << aircraftID
                    << " | Samples: " << rec.sampleCount
                    << " | Avg consumption: "
                    << std::fixed << std::setprecision(2) << gph << " GPH"
                    << " | Fuel remaining: "
                    << std::fixed << std::setprecision(2) << currFuel << " gal";
                log(oss.str());
            }
        }

        // ── SYS-020: end-of-flight signal ────────────────────────────────────
        if (pkt.isEOF)
        {
            break;
        }
    }

done:
    closesocket(clientSocket);

    if (!identified)
        return;

    // Finalise and save the flight record (SYS-020).
    std::lock_guard<std::mutex> lk(g_fleetMutex);
    FlightRecord& rec = g_fleet[aircraftID];
    rec.firstSample = true;       // ready for the next flight
    rec.lifetimeConsumed += rec.totalConsumed;
    rec.lifetimeElapsed += rec.totalElapsed;
    rec.totalFlights++;

    double flightGPH = (rec.totalElapsed > 0.0)
        ? (rec.totalConsumed / rec.totalElapsed) * 3600.0
        : 0.0;
    double lifetimeGPH = (rec.lifetimeElapsed > 0.0)
        ? (rec.lifetimeConsumed / rec.lifetimeElapsed) * 3600.0
        : 0.0;

    std::ostringstream oss;
    oss << "Aircraft " << aircraftID
        << " - Flight #" << rec.flightNumber << " ended."
        << " Flight avg: " << std::fixed << std::setprecision(2)
        << flightGPH << " GPH"
        << " | Lifetime avg: " << lifetimeGPH << " GPH"
        << " (" << rec.totalFlights << " flight(s))";
    log(oss.str());

    saveFinalAverage(rec);
}

// ─── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Optional: override port via first argument
    int port = SERVER_PORT;
    if (argc >= 2)
        port = std::atoi(argv[1]);

    // ── Init Winsock ──────────────────────────────────────────────────────────
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    // ── Open log file ─────────────────────────────────────────────────────────
    g_logFile.open("server_log.txt", std::ios::out | std::ios::app);

    // ── Create listening socket ───────────────────────────────────────────────
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
    {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // Allow address reuse so the server can restart quickly after a crash test.
    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(listenSock,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(listenSock, BACKLOG) == SOCKET_ERROR)
    {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    log("=== Telemetry Server started on port " + std::to_string(port) + " ===");
    log("Waiting for aircraft connections...\n");

    // ── Accept loop (SYS-001) ─────────────────────────────────────────────────
    while (g_running.load())
    {
        sockaddr_in clientAddr{};
        int         addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(listenSock,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &addrLen);
        if (clientSock == INVALID_SOCKET)
        {
            if (g_running.load())
                log("accept() error: " + std::to_string(WSAGetLastError()));
            continue;
        }

        char ipBuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        log("New connection from " + std::string(ipBuf) +
            ":" + std::to_string(ntohs(clientAddr.sin_port)));

        // Detach a thread for this client (SYS-001).
        std::thread(clientThread, clientSock).detach();
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    closesocket(listenSock);
    WSACleanup();
    log("Server shut down.");
    return 0;
}
