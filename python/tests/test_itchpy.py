"""Tests for the itchpy bindings.

Message bytes are packed here from the ITCH 5.0 spec with `struct`, separately
from the C++ decoder, so a decoder bug can't hide behind a matching test bug.
"""

import os
import pathlib
import struct
import tempfile
import unittest

from itchpy import BookStatus, Level, OrderBook, QuoteStream, Side

AAPL, MSFT = 7, 9


def frame(payload: bytes) -> bytes:
    return struct.pack(">H", len(payload)) + payload


def header(kind: bytes, locate: int, ts: int) -> bytes:
    # type, stock_locate, tracking_number, 6-byte timestamp
    return kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")


def add(locate, ts, ref, side, shares, stock, price) -> bytes:
    body = struct.pack(">Qc I 8s I", ref, side, shares, stock.encode().ljust(8), price)
    return frame(header(b"A", locate, ts) + body)


def execute(locate, ts, ref, shares) -> bytes:
    return frame(header(b"E", locate, ts) + struct.pack(">QIQ", ref, shares, 1))


def delete(locate, ts, ref) -> bytes:
    return frame(header(b"D", locate, ts) + struct.pack(">Q", ref))


class OrderBookTest(unittest.TestCase):
    def test_hand_worked_example(self):
        book = OrderBook()
        self.assertEqual(book.add(1001, Side.Buy, 1834500, 100), BookStatus.Ok)
        book.add(1002, Side.Sell, 1835000, 200)
        book.add(1003, Side.Buy, 1834500, 50)
        book.reduce(1001, 30)
        book.reduce(1003, 50)
        book.remove(1002)

        self.assertEqual(book.bids(), [Level(1834500, 70)])
        self.assertEqual(book.asks(), [])
        self.assertIsNone(book.best_ask())
        self.assertEqual(book.order_count, 1)

    def test_errors_come_back_as_statuses(self):
        book = OrderBook()
        book.add(1, Side.Buy, 1834500, 100)
        self.assertEqual(book.add(1, Side.Buy, 1834500, 100), BookStatus.DuplicateOrder)
        self.assertEqual(book.remove(99), BookStatus.UnknownOrder)

    def test_negative_values_are_rejected(self):
        with self.assertRaises(TypeError):
            OrderBook().add(1, Side.Buy, -1, 100)


class QuoteStreamTest(unittest.TestCase):
    def write(self, data: bytes) -> pathlib.Path:
        fd, name = tempfile.mkstemp(suffix=".itch")
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        self.addCleanup(os.remove, name)
        return pathlib.Path(name)

    def test_yields_top_of_book_changes_for_one_symbol(self):
        path = self.write(
            add(AAPL, 100, 1, b"B", 100, "AAPL", 1834500)
            + add(MSFT, 150, 2, b"B", 10, "MSFT", 1580000)
            + add(AAPL, 200, 3, b"B", 50, "AAPL", 1834000)  # behind the best bid
            + add(AAPL, 300, 4, b"S", 200, "AAPL", 1835000)
            + execute(AAPL, 400, 1, 30)
            + delete(AAPL, 500, 4)
        )
        stream = QuoteStream(path, "AAPL")
        quotes = [(q.timestamp, q.bid, q.ask) for q in stream]

        self.assertEqual(
            quotes,
            [
                (100, Level(1834500, 100), None),
                (300, Level(1834500, 100), Level(1835000, 200)),
                (400, Level(1834500, 70), Level(1835000, 200)),
                (500, Level(1834500, 70), None),
            ],
        )
        self.assertEqual(stream.book.order_count, 2)
        self.assertEqual((stream.malformed, stream.book_errors, stream.truncated), (0, 0, False))

    def test_accepts_str_paths(self):
        path = self.write(add(AAPL, 100, 1, b"S", 100, "AAPL", 1835000))
        self.assertEqual(len(list(QuoteStream(str(path), "AAPL"))), 1)

    def test_missing_file_raises(self):
        with self.assertRaises(FileNotFoundError):
            QuoteStream("/no/such/file.itch", "AAPL")


if __name__ == "__main__":
    unittest.main()
