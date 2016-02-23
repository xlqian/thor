#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <prime_server/http_protocol.hpp>
#include <prime_server/prime_server.hpp>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace prime_server;

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/constants.h>
#include <valhalla/baldr/json.h>

#include "thor/service.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;


namespace {
  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const headers_t::value_type JSON_MIME{"Content-type", "application/json;charset=utf-8"};
  const headers_t::value_type JS_MIME{"Content-type", "application/javascript;charset=utf-8"};

  json::ArrayPtr locations(const std::vector<PathLocation>& correlated) {
    auto input_locs = json::array({});
    for(size_t i = 0; i < correlated.size(); i++) {
      input_locs->emplace_back(
        json::map({
          {"lat", json::fp_t{correlated[i].latlng_.lat(), 6}},
          {"lon", json::fp_t{correlated[i].latlng_.lng(), 6}}
        })
      );
    }
    return input_locs;
  }
}

namespace valhalla {
  namespace thor {

  worker_t::result_t thor_worker_t::trip_path(const std::string &costing, const std::string &request_str, const boost::optional<int> &date_time_type){
    worker_t::result_t result{true};
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    // Forward the original request
    result.messages.emplace_back(request_str);

    if (date_time_type && *date_time_type == 2) {
     return getPathArriveBy(correlated, costing, request_str, result);
    } else {
     return getPathDepartFrom(correlated, costing, date_time_type, request_str, result);
    }
    //get processing time for thor
    auto e = std::chrono::system_clock::now();
    std::chrono::duration<float, std::milli> elapsed_time = e - s;
    //log request if greater than X (ms)
    if ((elapsed_time.count() / correlated.size()) > long_request_route) {
     LOG_WARN("thor::route get_trip_path elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
     LOG_WARN("thor::route get_trip_path exceeded threshold::"+ request_str);
     midgard::logging::Log("valhalla_thor_long_request_route", " [ANALYTICS] ");
    }
    return result;
  }

  worker_t::result_t thor_worker_t::getPathArriveBy(std::vector<PathLocation>& correlated, const std::string &costing, const std::string &request_str, worker_t::result_t result) {
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    // For each pair of origin/destination
    bool prior_is_node = false;
    baldr::GraphId through_edge;
    std::vector<baldr::PathLocation> through_loc;
    std::vector<thor::PathInfo> path_edges;
    std::string origin_date_time;

    std::list<std::string> messages;
    baldr::PathLocation& last_break_dest = *correlated.rbegin();

    for(auto path_location = ++correlated.crbegin(); path_location != correlated.crend(); ++path_location) {
      auto origin = *path_location;
      auto destination = *std::prev(path_location);

      // Through edge is valid if last orgin was "through"
      if (through_edge.Is_Valid()) {
        UpdateOrigin(origin, prior_is_node, through_edge);
      } else {
        last_break_dest = destination;
      }

      // Get the algorithm type for this location pair
      thor::PathAlgorithm* path_algorithm;

      if (costing == "multimodal") {
        path_algorithm = &multi_modal_astar;
      } else if (costing == "pedestrian" || costing == "bicycle") {
        // Use bidirectional A* for pedestrian and bicycle if over 10km
        float dist = origin.latlng_.Distance(destination.latlng_);
        path_algorithm = (dist > 10000.0f) ? &bidir_astar : &astar;
      } else {
        path_algorithm = &astar;
      }

      // Get best path
      if (path_edges.size() == 0) {
        GetPath(path_algorithm, origin, destination, path_edges);
        if (path_edges.size() == 0) {
          throw std::runtime_error("No path could be found for input");
        }
      } else {
        // Get the path in a temporary vector
        std::vector<thor::PathInfo> temp_path;
        GetPath(path_algorithm, origin, destination, temp_path);
        if (temp_path.size() == 0) {
          throw std::runtime_error("No path could be found for input");
        }

        // Append the temp_path edges to path_edges, adding the elapsed
        // time from the end of the current path. If continuing along the
        // same edge, remove the prior so we do not get a duplicate edge.
        uint32_t t = path_edges.back().elapsed_time;
        if (temp_path.front().edgeid == path_edges.back().edgeid) {
          path_edges.pop_back();
        }
        for (auto edge : temp_path) {
          edge.elapsed_time += t;
          path_edges.emplace_back(edge);
        }
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (origin.stoptype_ == Location::StopType::BREAK ||
          path_location == --correlated.crend()) {

          if (!origin_date_time.empty())
            last_break_dest.date_time_ = origin_date_time;

          // Form output information based on path edges
          auto trip_path = thor::TripPathBuilder::Build(reader, path_edges,
                              origin, last_break_dest, through_loc);

          if (origin.date_time_)
            origin_date_time = *origin.date_time_;

          // The protobuf path
          messages.emplace_front(trip_path.SerializeAsString());

          // Clear path edges and set through edge to invalid
          path_edges.clear();
          through_edge = baldr::GraphId();
      } else {
          // This is a through location. Save last edge as the through_edge
          prior_is_node = origin.IsNode();
          through_edge = path_edges.back().edgeid;

          // Add to list of through locations for this leg
          through_loc.emplace_back(origin);
      }

      // If we have another one coming we need to clear
      if (--correlated.crend() != path_location)
        path_algorithm->Clear();
    }

    for (const auto msg : messages)
      result.messages.emplace_back(msg);

    //get processing time for thor
    auto e = std::chrono::system_clock::now();
    std::chrono::duration<float, std::milli> elapsed_time = e - s;
    //log request if greater than X (ms)
    if ((elapsed_time.count() / correlated.size()) > long_request_route) {
      LOG_WARN("thor::route getPathArriveBy elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
      LOG_WARN("thor::route getPathArriveBy exceeded threshold::"+ request_str);
      midgard::logging::Log("valhalla_thor_long_request_route", " [ANALYTICS] ");
    }
    return result;
  }

  worker_t::result_t thor_worker_t::getPathDepartFrom(std::vector<PathLocation>& correlated, const std::string &costing, const boost::optional<int> &date_time_type, const std::string &request_str, worker_t::result_t result) {
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    bool prior_is_node = false;
    std::vector<baldr::PathLocation> through_loc;
    baldr::GraphId through_edge;
    std::vector<thor::PathInfo> path_edges;
    std::string origin_date_time, dest_date_time;

    baldr::PathLocation& last_break_origin = correlated[0];
    for(auto path_location = ++correlated.cbegin(); path_location != correlated.cend(); ++path_location) {
      auto origin = *std::prev(path_location);
      auto destination = *path_location;

      if (date_time_type && (*date_time_type == 0 || *date_time_type == 1) &&
          !dest_date_time.empty() && origin.stoptype_ == Location::StopType::BREAK)
        origin.date_time_ = dest_date_time;

      // Through edge is valid if last destination was "through"
      if (through_edge.Is_Valid()) {
        UpdateOrigin(origin, prior_is_node, through_edge);
      } else {
        last_break_origin = origin;
      }

      // Get the algorithm type for this location pair
      thor::PathAlgorithm* path_algorithm;

      if (costing == "multimodal") {
        path_algorithm = &multi_modal_astar;
      } else if (costing == "pedestrian" || costing == "bicycle") {
        // Use bidirectional A* for pedestrian and bicycle if over 10km
        float dist = origin.latlng_.Distance(destination.latlng_);
        path_algorithm = (dist > 10000.0f) ? &bidir_astar : &astar;
      } else {
        path_algorithm = &astar;
      }

      // Get best path
      if (path_edges.size() == 0) {
        GetPath(path_algorithm, origin, destination, path_edges);
        if (path_edges.size() == 0) {
          throw std::runtime_error("No path could be found for input");
        }

        if (date_time_type && *date_time_type == 0 && origin_date_time.empty() &&
            origin.stoptype_ == Location::StopType::BREAK)
          last_break_origin.date_time_ = origin.date_time_;
      } else {
        // Get the path in a temporary vector
        std::vector<thor::PathInfo> temp_path;
        GetPath(path_algorithm, origin, destination, temp_path);
        if (temp_path.size() == 0) {
          throw std::runtime_error("No path could be found for input");
        }

        if (date_time_type && *date_time_type == 0 && origin_date_time.empty() &&
            origin.stoptype_ == Location::StopType::BREAK)
          last_break_origin.date_time_ = origin.date_time_;

        // Append the temp_path edges to path_edges, adding the elapsed
        // time from the end of the current path. If continuing along the
        // same edge, remove the prior so we do not get a duplicate edge.
        uint32_t t = path_edges.back().elapsed_time;
        if (temp_path.front().edgeid == path_edges.back().edgeid) {
          path_edges.pop_back();
        }
        for (auto edge : temp_path) {
          edge.elapsed_time += t;
          path_edges.emplace_back(edge);
        }
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (destination.stoptype_ == Location::StopType::BREAK ||
          path_location == --correlated.cend()) {
          // Form output information based on path edges
          auto trip_path = thor::TripPathBuilder::Build(reader, path_edges,
                                                        last_break_origin, destination, through_loc);

          if (date_time_type) {
            origin_date_time = *last_break_origin.date_time_;
            dest_date_time = *destination.date_time_;
          }
          // The protobuf path
          result.messages.emplace_back(trip_path.SerializeAsString());

          // Clear path edges and set through edge to invalid
          path_edges.clear();
          through_edge = baldr::GraphId();
      } else {
          // This is a through location. Save last edge as the through_edge
          prior_is_node = destination.IsNode();
          through_edge = path_edges.back().edgeid;

          // Add to list of through locations for this leg
          through_loc.emplace_back(destination);
      }

      // If we have another one coming we need to clear
      if (--correlated.cend() != path_location)
        path_algorithm->Clear();
    }
    //get processing time for thor
    auto e = std::chrono::system_clock::now();
    std::chrono::duration<float, std::milli> elapsed_time = e - s;
    //log request if greater than X (ms)
    if ((elapsed_time.count() / correlated.size()) > long_request_route) {
      LOG_WARN("thor::route getPathDepartFrom elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
      LOG_WARN("thor::route getPathDepartFrom exceeded threshold::"+ request_str);
      midgard::logging::Log("valhalla_thor_long_request_route", " [ANALYTICS] ");
    }
    return result;
  }

  }
}