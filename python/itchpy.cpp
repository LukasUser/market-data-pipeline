// Python bindings for the order book and quote stream, so strategies and
// research can be written in Python on top of the C++ replay.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "itch/order_book.hpp"
#include "itch/quote_stream.hpp"

namespace py = pybind11;

namespace {

std::string price_repr(itch::Price p) {
  // Fixed point printed exactly: 1834500 → "183.4500".
  return std::to_string(p / 10000) + "." + std::to_string(10000 + p % 10000).substr(1);
}

std::string level_repr(const std::optional<itch::Level>& level) {
  if (!level) return "None";
  return "Level(" + price_repr(level->price) + " x " + std::to_string(level->shares) + ")";
}

// Owns the input a QuoteStream reads from, so Python holds a single object.
// "-" reads standard input, for piping from gunzip.
class PyQuoteStream {
 public:
  PyQuoteStream(const std::filesystem::path& path, std::string symbol)
      : file_(open(path)), stream_(file_ ? *file_ : std::cin, std::move(symbol)) {}

  std::optional<itch::Quote> next() { return stream_.next(); }
  const itch::QuoteStream& stream() const { return stream_; }

 private:
  static std::unique_ptr<std::ifstream> open(const std::filesystem::path& path) {
    if (path == "-") return nullptr;
    auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*file) {
      PyErr_SetString(PyExc_FileNotFoundError, ("cannot open " + path.string()).c_str());
      throw py::error_already_set();
    }
    return file;
  }

  // Declared before stream_, so the file is open before the stream reads it.
  std::unique_ptr<std::ifstream> file_;
  itch::QuoteStream stream_;
};

}  // namespace

PYBIND11_MODULE(itchpy, m) {
  m.doc() = "NASDAQ ITCH 5.0 order book replay. Prices are fixed point with 4 decimals.";

  py::enum_<itch::Side>(m, "Side")
      .value("Buy", itch::Side::Buy)
      .value("Sell", itch::Side::Sell);

  py::enum_<itch::BookStatus>(m, "BookStatus")
      .value("Ok", itch::BookStatus::Ok)
      .value("UnknownOrder", itch::BookStatus::UnknownOrder)
      .value("DuplicateOrder", itch::BookStatus::DuplicateOrder)
      .value("Overfill", itch::BookStatus::Overfill);

  py::class_<itch::Level>(m, "Level")
      .def(py::init<itch::Price, itch::Shares>(), py::arg("price"), py::arg("shares"))
      .def_readonly("price", &itch::Level::price)
      .def_readonly("shares", &itch::Level::shares)
      .def("__eq__", [](const itch::Level& a, const itch::Level& b) { return a == b; })
      .def("__repr__", [](const itch::Level& l) { return level_repr(l); });

  py::class_<itch::Quote>(m, "Quote")
      .def_readonly("timestamp", &itch::Quote::timestamp, "Nanoseconds since midnight.")
      .def_readonly("bid", &itch::Quote::bid, "Best bid Level, or None if no bids rest.")
      .def_readonly("ask", &itch::Quote::ask, "Best ask Level, or None if no asks rest.")
      .def("__eq__", [](const itch::Quote& a, const itch::Quote& b) { return a == b; })
      .def("__repr__", [](const itch::Quote& q) {
        return "Quote(t=" + std::to_string(q.timestamp) + ", bid=" + level_repr(q.bid) +
               ", ask=" + level_repr(q.ask) + ")";
      });

  py::class_<itch::OrderBook>(m, "OrderBook")
      .def(py::init<>())
      .def("add", &itch::OrderBook::add, py::arg("ref"), py::arg("side"), py::arg("price"),
           py::arg("shares"))
      .def("reduce", &itch::OrderBook::reduce, py::arg("ref"), py::arg("shares"))
      .def("remove", &itch::OrderBook::remove, py::arg("ref"))
      .def("replace", &itch::OrderBook::replace, py::arg("old_ref"), py::arg("new_ref"),
           py::arg("price"), py::arg("shares"))
      .def("best_bid", &itch::OrderBook::best_bid)
      .def("best_ask", &itch::OrderBook::best_ask)
      .def("bids", &itch::OrderBook::bids, py::arg("depth") = 5)
      .def("asks", &itch::OrderBook::asks, py::arg("depth") = 5)
      .def_property_readonly("order_count", &itch::OrderBook::order_count);

  py::class_<PyQuoteStream>(m, "QuoteStream",
                            "Iterates over one symbol's top-of-book changes in an ITCH file.")
      .def(py::init<const std::filesystem::path&, std::string>(), py::arg("path"),
           py::arg("symbol"))
      .def("__iter__", [](PyQuoteStream& s) -> PyQuoteStream& { return s; },
           py::return_value_policy::reference_internal)
      .def("__next__",
           [](PyQuoteStream& s) {
             auto quote = s.next();
             if (!quote) throw py::stop_iteration();
             return *quote;
           })
      .def_property_readonly(
          "book", [](const PyQuoteStream& s) { return s.stream().book(); },
          py::return_value_policy::reference_internal,
          "The symbol's full book as of the last quote, or None before its first order.")
      .def_property_readonly("malformed", [](const PyQuoteStream& s) { return s.stream().malformed(); })
      .def_property_readonly("book_errors", [](const PyQuoteStream& s) { return s.stream().book_errors(); })
      .def_property_readonly("truncated", [](const PyQuoteStream& s) { return s.stream().truncated(); });
}
