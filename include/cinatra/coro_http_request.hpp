#pragma once

#include <charconv>
#include <regex>

#include "async_simple/coro/Lazy.h"
#include "define.h"
#include "http_parser.hpp"
#include "ws_define.h"

namespace cinatra {

inline std::vector<std::string_view> split_sv(std::string_view s,
                                              std::string_view delimiter) {
  size_t start = 0;
  size_t end = s.find_first_of(delimiter);

  std::vector<std::string_view> output;

  while (end <= std::string_view::npos) {
    output.emplace_back(s.substr(start, end - start));

    if (end == std::string_view::npos)
      break;

    start = end + 1;
    end = s.find_first_of(delimiter, start);
  }

  return output;
}

inline std::string_view trim_sv(std::string_view v) {
  v.remove_prefix((std::min)(v.find_first_not_of(" "), v.size()));
  v.remove_suffix((std::min)(v.size() - v.find_last_not_of(" ") - 1, v.size()));
  return v;
}

inline std::vector<std::pair<int, int>> parse_ranges(std::string_view range_str,
                                                     size_t file_size,
                                                     bool& is_valid) {
  range_str = trim_sv(range_str);
  if (range_str.empty()) {
    return {{0, file_size - 1}};
  }

  if (range_str.find("--") != std::string_view::npos) {
    is_valid = false;
    return {};
  }

  if (range_str == "-") {
    return {{0, file_size - 1}};
  }

  std::vector<std::pair<int, int>> vec;
  auto ranges = split_sv(range_str, ",");
  for (auto range : ranges) {
    auto sub_range = split_sv(range, "-");
    auto fist_range = trim_sv(sub_range[0]);

    int start = 0;
    if (fist_range.empty()) {
      start = -1;
    }
    else {
      auto [ptr, ec] = std::from_chars(
          fist_range.data(), fist_range.data() + fist_range.size(), start);
      if (ec != std::errc{}) {
        is_valid = false;
        return {};
      }
    }

    int end = 0;
    if (sub_range.size() == 1) {
      end = file_size - 1;
    }
    else {
      auto second_range = trim_sv(sub_range[1]);
      if (second_range.empty()) {
        end = file_size - 1;
      }
      else {
        auto [ptr, ec] =
            std::from_chars(second_range.data(),
                            second_range.data() + second_range.size(), end);
        if (ec != std::errc{}) {
          is_valid = false;
          return {};
        }
      }
    }

    if (start > 0 && (start >= file_size || start == end)) {
      // out of range
      is_valid = false;
      return {};
    }

    if (end > 0 && end >= file_size) {
      end = file_size - 1;
    }

    if (start == -1) {
      start = file_size - end;
      end = file_size - 1;
    }

    vec.push_back({start, end});
  }
  return vec;
}

class coro_http_connection;
class coro_http_request {
 public:
  coro_http_request(http_parser& parser, coro_http_connection* conn)
      : parser_(parser), conn_(conn) {}

  std::string_view get_header_value(std::string_view key) {
    auto headers = parser_.get_headers();
    for (auto& header : headers) {
      if (iequal0(header.name, key)) {
        return header.value;
      }
    }

    return {};
  }

  std::string_view get_query_value(std::string_view key) {
    return parser_.get_query_value(key);
  }

  std::string get_decode_query_value(std::string_view key) {
    auto value = parser_.get_query_value(key);
    if (value.empty()) {
      return "";
    }

    return code_utils::get_string_by_urldecode(value);
  }

  std::span<http_header> get_headers() const { return parser_.get_headers(); }

  const auto& get_queries() const { return parser_.queries(); }

  void set_body(std::string& body) {
    body_ = body;
    auto type = get_content_type();
    if (type == content_type::urlencoded) {
      parser_.parse_query(body_);
    }
  }

  std::string_view get_body() const { return body_; }

  bool is_chunked() { return parser_.is_chunked(); }

  bool is_resp_ranges() { return parser_.is_resp_ranges(); }

  bool is_req_ranges() { return parser_.is_req_ranges(); }

  content_type get_content_type() {
    if (is_chunked())
      return content_type::chunked;

    auto content_type = get_header_value("content-type");
    if (!content_type.empty()) {
      if (content_type.find("application/x-www-form-urlencoded") !=
          std::string_view::npos) {
        return content_type::urlencoded;
      }
      else if (content_type.find("multipart/form-data") !=
               std::string_view::npos) {
        return content_type::multipart;
      }
      else if (content_type.find("application/octet-stream") !=
               std::string_view::npos) {
        return content_type::octet_stream;
      }
      else {
        return content_type::string;
      }
    }

    if (is_websocket_) {
      return content_type::websocket;
    }

    return content_type::unknown;
  }

  std::string_view get_boundary() {
    auto content_type = get_header_value("content-type");
    if (content_type.empty()) {
      return {};
    }
    return content_type.substr(content_type.rfind("=") + 1);
  }

  coro_http_connection* get_conn() { return conn_; }

  bool is_upgrade() {
    auto h = get_header_value("Connection");
    if (h.empty())
      return false;

    auto u = get_header_value("Upgrade");
    if (u.empty())
      return false;

    if (h != UPGRADE)
      return false;

    if (u != WEBSOCKET)
      return false;

    auto sec_ws_key = get_header_value("sec-websocket-key");
    if (sec_ws_key.empty() || sec_ws_key.size() != 24)
      return false;

    is_websocket_ = true;
    return true;
  }

  std::unordered_map<std::string, std::string> params_;
  std::smatch matches_;

 private:
  http_parser& parser_;
  std::string_view body_;
  coro_http_connection* conn_;
  bool is_websocket_;
};
}  // namespace cinatra