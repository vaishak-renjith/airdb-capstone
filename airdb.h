#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace crow { namespace json { struct wvalue; } }

struct Airline {
    int         id = -1;
    std::string name;
    std::string alias;
    std::string iata;   // 2-letter
    std::string icao;   // 3-letter
    std::string callsign;
    std::string country;
    std::string active; // "Y"/"N"

    crow::json::wvalue toJSON() const;
};

struct Airport {
    int         id = -1;
    std::string name;
    std::string city;
    std::string country;
    std::string iata;     // 3-letter
    std::string icao;     // 4-letter
    double      latitude = 0.0;
    double      longitude = 0.0;
    int         altitude_ft = 0;
    double      tz_offset = 0.0; // hours
    std::string dst;      // E, A, S, O, Z, N
    std::string tz_db;    // e.g. "America/Los_Angeles"
    std::string type;
    std::string source;

    crow::json::wvalue toJSON() const;
};

struct Route {
    std::string airline_iata; // could be blank; file may have airline code or id
    int         airline_id = -1;
    std::string src_iata;
    int         src_id = -1;
    std::string dst_iata;
    int         dst_id = -1;
    std::string codeshare; // "Y" or ""
    int         stops = 0;
    std::string equipment;

    crow::json::wvalue toJSON() const;
};

class AirTravelDB {
public:
    // Bulk access (used for suggestions / name contains)
    std::vector<Airline> GetAllAirlines() const;
    std::vector<Airport> GetAllAirports() const;

    // Lookups
    std::shared_ptr<Airline> GetAirlineByIATA(const std::string& iata) const;
    std::shared_ptr<Airline> GetAirlineByICAO(const std::string& icao) const;
    std::shared_ptr<Airline> GetAirlineByID(int id) const;

    std::shared_ptr<Airport> GetAirportByIATA(const std::string& iata) const;
    std::shared_ptr<Airport> GetAirportByICAO(const std::string& icao) const;
    std::shared_ptr<Airport> GetAirportByID(int id) const;

    // Loaders
    bool LoadAirlinesCSV(const std::string& path);
    bool LoadAirportsCSV(const std::string& path);
    bool LoadRoutesCSV(const std::string& path);

    // Routes
    std::vector<Route> GetRoutesFromTo(const std::string& src_iata,
        const std::string& dst_iata) const;
    std::vector<Route> SearchRoutes(const std::string& token) const;

    // Geo
    double CalculateDistanceKm(double lat1, double lon1,
        double lat2, double lon2) const;
    std::vector<std::pair<std::shared_ptr<Airport>, int>>
        GetAirportsWithinRadiusKm(double lat, double lon, double radius_km) const;

    // Routes
    const std::vector<Route>& GetAllRoutes() const;

private:
    // parsing helpers used by loaders
    static std::vector<std::string> parseCSVLine(const std::string& line);
    static std::string cleanField(const std::string& s);

    // data
    std::unordered_map<std::string, std::shared_ptr<Airline>> airlines_by_iata_;
    std::unordered_map<int, std::shared_ptr<Airline>>         airlines_by_id_;
    std::unordered_map<std::string, std::shared_ptr<Airline>> airlines_by_icao_;

    std::unordered_map<std::string, std::shared_ptr<Airport>> airports_by_iata_;
    std::unordered_map<int, std::shared_ptr<Airport>>         airports_by_id_;
    std::unordered_map<std::string, std::shared_ptr<Airport>> airports_by_icao_;

    std::vector<Route> routes_;
    mutable std::mutex mtx_;
};

