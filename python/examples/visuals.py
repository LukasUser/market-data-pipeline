"""Makes the README's images from real results, in light and dark variants.

    PYTHONPATH=build-release:python python3 python/examples/visuals.py ~/data/itch/12302019 docs/img

Reads the per-symbol files from itch_split and results.jsonl from runner.py
in that folder. Charts are SVG (sharp at any size); the order-book replay is a
GIF. Needs matplotlib and Pillow, which nothing else in the project uses.
"""

import json
import logging
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Rectangle  # noqa: E402
from PIL import Image  # noqa: E402

from backtest import NS, OPEN, next_move_by_imbalance  # noqa: E402
from itchpy import PassiveSim, QuoteStream, Side  # noqa: E402
from market_making import JoinBest, make_markets  # noqa: E402

logging.getLogger("matplotlib.font_manager").setLevel(logging.ERROR)

# Reference palette (validated for color-blind safety in both modes; light
# aqua is below 3:1 on the surface, so it is always paired with a text label).
THEMES = {
    "light": dict(page="#ffffff", surface="#fcfcfb", border="#e1e0d9", ink="#0b0b0b", ink2="#52514e",
                  muted="#898781", grid="#ecebe6", axis="#c3c2b7", band="#f2f1ec",
                  blue="#2a78d6", orange="#eb6834", aqua="#1baf7a", red="#e34948"),
    "dark": dict(page="#0d1117", surface="#1a1a19", border="#2c2c2a", ink="#f0efec", ink2="#c3c2b7",
                 muted="#898781", grid="#262624", axis="#383835", band="#232322",
                 blue="#3987e5", orange="#d95926", aqua="#199e70", red="#e66767"),
}
TICK = 100  # $0.01 in price units
MINUS = "−"
SOURCE = "Source: NASDAQ TotalView-ITCH 5.0, December 30, 2019  ·  github.com/LukasUser/market-data-pipeline"

plt.rcParams.update({
    "font.family": "sans-serif", "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
    "svg.fonttype": "path", "axes.unicode_minus": True,
})


def cents(x: float, digits: int = 2) -> str:
    return f"{'+' if x > 0 else MINUS if x < 0 else ''}{abs(x):.{digits}f}¢"


def dollars(price: int) -> str:
    return f"${price / 10_000:,.2f}"


# --- layout helpers: everything is placed in pixels from the top-left -------------

class Canvas:
    def __init__(self, theme: dict, width: int, height: int, dpi: int = 100, page: bool = False):
        self.t, self.w, self.h = theme, width, height
        self.fig = plt.figure(figsize=(width / 100, height / 100), dpi=dpi)
        if page:  # GIFs have no transparency to speak of, so paint GitHub's page around the card
            self.fig.patch.set_facecolor(theme["page"])
        else:
            self.fig.patch.set_alpha(0)
        r = 14
        self.fig.patches.append(FancyBboxPatch(
            (1 / width, 1 / height), 1 - 2 / width, 1 - 2 / height,
            boxstyle=f"round,pad=0,rounding_size={r / width}", mutation_aspect=width / height,
            transform=self.fig.transFigure, facecolor=theme["surface"], edgecolor=theme["border"],
            linewidth=1, zorder=-10))

    def text(self, x, y, s, size=11, color="ink", weight="normal", ha="left", va="top", **kw):
        return self.fig.text(x / self.w, 1 - y / self.h, s, fontsize=size, color=self.t.get(color, color),
                             fontweight=weight, ha=ha, va=va, **kw)

    def axes(self, x, y, width, height):
        ax = self.fig.add_axes([x / self.w, 1 - (y + height) / self.h, width / self.w, height / self.h])
        ax.set_facecolor("none")
        for side in ax.spines.values():
            side.set_visible(False)
        ax.tick_params(length=0, colors=self.t["muted"], labelsize=10)
        return ax

    def header(self, title, subtitle):
        self.text(36, 30, title, size=17, weight="bold")
        self.text(36, 60, subtitle, size=11, color="ink2", linespacing=1.45)

    def footer(self, s=SOURCE):
        self.text(36, self.h - 22, s, size=8.5, color="muted", va="bottom")

    def save(self, path: Path):
        self.fig.savefig(path, transparent=True)
        plt.close(self.fig)

    def frame(self) -> Image.Image:
        self.fig.canvas.draw()
        im = Image.frombuffer("RGBA", self.fig.canvas.get_width_height(), self.fig.canvas.buffer_rgba())
        plt.close(self.fig)
        return im.convert("RGB")


def rounded_bar(ax, x, y, width, height, color, radius=4, square=None, gap=None, zorder=2):
    """A bar with rounded corners in data coordinates (limits must already be
    set). `square` ('left', 'right', 'bottom', 'top') keeps the baseline side
    square; `gap` draws a surface-colored outline that separates neighbours."""
    if width < 0:
        x, width = x + width, -width
    if height < 0:
        y, height = y + height, -height
    (x0, y0), (x1, y1) = ax.transData.transform([(0, 0), (1, 1)])
    sx, sy = abs(x1 - x0), abs(y1 - y0)  # pixels per data unit
    r = min(radius, width * sx / 2, height * sy / 2)
    if r <= 0.01:
        return
    ax.add_patch(FancyBboxPatch((x, y), width, height, boxstyle=f"round,pad=0,rounding_size={r / sx}",
                                mutation_aspect=sx / sy, facecolor=color, edgecolor=gap or "none",
                                linewidth=1.5 if gap else 0, zorder=zorder))
    rx, ry = r / sx, r / sy
    cover = {"bottom": (x, y, width, ry), "top": (x, y + height - ry, width, ry),
             "left": (x, y, rx, height), "right": (x + width - rx, y, rx, height)}.get(square)
    if cover:
        ax.add_patch(Rectangle(cover[:2], *cover[2:], facecolor=color, edgecolor="none", zorder=zorder))


# --- data ---------------------------------------------------------------------------

def aapl_market_making(day: Path):
    sim = PassiveSim(day / "AAPL.itch", "AAPL", latency_ns=10_000)
    return make_markets(sim, JoinBest(size=100, max_position=500), rebate_per_share=20)


def opening_price(path: Path, symbol: str) -> float:
    for q in QuoteStream(path, symbol):
        if q.timestamp >= OPEN and q.bid and q.ask:
            return (q.bid.price + q.ask.price) / 2 / 10_000
    return float("nan")


def stock_bps(day: Path) -> dict[int, tuple[list[tuple[str, float]], int, int]]:
    """Per latency (join strategy): each stock's profit per dollar traded in
    basis points, how many stocks made money, and how many were run."""
    records = [json.loads(line) for line in (day / "results.jsonl").read_text().splitlines()]
    latest = {(r["symbol"], r["strategy"], r["latency_us"]): r for r in records if "error" not in r}
    prices = {s: opening_price(day / f"{s}.itch", s) for s, _, _ in latest}
    out = {}
    for lat in (10, 1000):
        runs = [(s, r) for (s, strat, l), r in latest.items() if strat == "join" and l == lat]
        bps = [(s, 10_000 * r["pnl"] / r["shares"] / prices[s]) for s, r in runs if r["shares"]]
        out[lat] = (bps, sum(1 for _, r in runs if r["pnl"] > 0), len(runs))
    return out


# --- static charts (SVG) -----------------------------------------------------------

def queue_audit(out: Path):
    # Counts from the full-day queue audit (tools/itch_queue) over all 8,892 stocks.
    front, total = 5_672_614, 5_683_179
    share = 100 * front / total
    for mode, t in THEMES.items():
        c = Canvas(t, 1000, 330)
        c.header("Our matching engine queues orders like NASDAQ does",
                 "Every NASDAQ execution, checked against the order our engine had at the front of its queue.")
        c.text(36, 108, f"{share:.2f}%", size=58, weight="bold")
        c.text(40, 182, f"of {total:,} regular-hours executions hit the\nfront of our queue",
               size=11.5, color="ink2", linespacing=1.45)
        for i, (n, label) in enumerate([("0", "executions at a\nworse price"), ("0", "orders we\ndidn't know"),
                                        ("0", "book errors across\n8,892 stocks")]):
            x = 470 + i * 170
            c.text(x, 116, n, size=30, weight="bold")
            c.text(x, 160, label, size=10.5, color="ink2", linespacing=1.4)
        ax = c.axes(36, 252, 928, 12)
        ax.set_xlim(0, 100)
        ax.set_ylim(0, 1)
        ax.set_xticks([])
        ax.set_yticks([])
        rounded_bar(ax, 0, 0, 100, 1, t["grid"], radius=6)
        rounded_bar(ax, 0, 0, share, 1, t["blue"], radius=6)
        c.text(964, 274, f"{100 - share:.2f}% behind other orders at the same price", size=9.5,
               color="muted", ha="right")
        c.footer()
        c.save(out / f"queue-audit-{mode}.svg")


def imbalance(day: Path, out: Path):
    rows = next_move_by_imbalance(list(QuoteStream(day / "AAPL.itch", "AAPL")))
    labels = [f"{lo:.1f}–{hi:.1f}" for lo, hi, _, _ in rows]
    shares = [100 * up / (up + down) for _, _, up, down in rows]
    for mode, t in THEMES.items():
        c = Canvas(t, 1000, 540)
        c.header("Order-book imbalance predicts AAPL's next price move",
                 "How often the next mid-price change is up, by top-of-book imbalance: bid shares ÷ "
                 "all shares at the best bid and ask.\nRegular hours, 09:30–16:00.")
        ax = c.axes(36, 130, 928, 300)
        ax.set_xlim(-0.6, len(rows) - 0.4)
        ax.set_ylim(0, 72)
        ax.set_yticks([])
        ax.set_xticks(range(len(rows)), labels, fontsize=11, color=t["ink2"])
        ax.tick_params(axis="x", pad=8)
        for i, s in enumerate(shares):
            rounded_bar(ax, i - 0.3, 0, 0.6, s, t["blue"], radius=5, square="bottom")
            ax.text(i, s + 1.6, f"{s:.1f}%", ha="center", va="bottom", fontsize=13, fontweight="bold",
                    color=t["ink"])
        ax.axhline(0, color=t["axis"], linewidth=1.2)
        ax.axhline(50, color=t["muted"], linewidth=1, linestyle=(0, (4, 4)), zorder=3)
        ax.text(-0.55, 51.2, "50%: a coin flip", fontsize=10, color=t["ink2"], va="bottom")
        c.text(36, 470, r"$\leftarrow$  sellers outweigh buyers", size=10, color="muted")
        c.text(964, 470, r"buyers outweigh sellers  $\rightarrow$", size=10, color="muted", ha="right")
        c.footer()
        c.save(out / f"imbalance-{mode}.svg")


def waterfall(r, out: Path):
    n = r.shares_filled
    spread, adverse = 100 * r.spread_captured / n, 100 * r.markout_1s / n
    rebates, net = 100 * r.rebates / n, 100 * r.pnl / n
    later = net - spread - adverse - rebates  # moves after the first second, and the 16:00 close-out
    steps = [("Spread\ncaptured", spread), ("Adverse selection\nin the first second", adverse),
             ("Later moves and\n16:00 close-out", later), ("NASDAQ\nrebate", rebates)]
    for mode, t in THEMES.items():
        c = Canvas(t, 1000, 540)
        c.header("Resting orders earn the spread, then lose it to adverse selection",
                 f"AAPL market making at the best bid and ask with 10 µs latency. "
                 f"Profit per share filled, {n:,} shares.")
        ax = c.axes(90, 110, 860, 330)
        ax.set_xlim(-0.6, len(steps) + 0.6)
        ax.set_ylim(-1.8, 1.5)
        ax.set_yticks([-1.5, -1, -0.5, 0, 0.5, 1, 1.5])
        ax.yaxis.set_major_formatter(lambda v, _: cents(v, 1) if v else "0")
        ax.grid(axis="y", color=t["grid"], linewidth=0.8)
        ax.set_axisbelow(True)
        ax.set_xticks(range(len(steps) + 1), [s for s, _ in steps] + ["Net per\nshare"], fontsize=10.5,
                      color=t["ink2"], linespacing=1.35)
        ax.tick_params(axis="x", pad=10)
        level = 0.0
        for i, (_, value) in enumerate(steps):
            rounded_bar(ax, i - 0.28, level, 0.56, value, t["blue"] if value >= 0 else t["red"], radius=5)
            ax.text(i, max(level, level + value) + 0.07, cents(value), ha="center", va="bottom", fontsize=12.5,
                    fontweight="bold", color=t["ink"])
            ax.plot([i + 0.28, i + 0.72], [level + value] * 2, color=t["muted"], linewidth=1,
                    linestyle=(0, (2, 2)), zorder=1)
            level += value
        rounded_bar(ax, len(steps) - 0.28, 0, 0.56, net, t["ink2"], radius=5, square="top")
        ax.text(len(steps), 0.07, cents(net), ha="center", va="bottom", fontsize=12.5, fontweight="bold",
                color=t["ink"])
        ax.axhline(0, color=t["axis"], linewidth=1.2, zorder=1)
        ax.axhline(-1.36, color=t["muted"], linewidth=1, linestyle=(0, (4, 4)), zorder=1)
        ax.text(len(steps) + 0.55, -1.42, f"Market orders: {cents(-1.36)}", fontsize=10, color=t["ink2"],
                ha="right", va="top")
        c.footer()
        c.save(out / f"pnl-waterfall-{mode}.svg")


def beeswarm(xs_px: list[float], diameter: float) -> list[float]:
    """Vertical offsets (pixels) that stack dots without overlap, closest to the center line first."""
    placed: list[tuple[float, float]] = []
    offsets = [0.0] * len(xs_px)
    for i in sorted(range(len(xs_px)), key=lambda i: xs_px[i]):
        x = xs_px[i]
        near = [(px, py) for px, py in placed if abs(px - x) < diameter]
        for k in range(200):
            y = ((k + 1) // 2) * diameter * 0.92 * (1 if k % 2 else -1)
            if all((px - x) ** 2 + (py - y) ** 2 >= (diameter * 0.98) ** 2 for px, py in near):
                break
        placed.append((x, y))
        offsets[i] = y
    return offsets


def stocks(day: Path, out: Path):
    data = stock_bps(day)
    lim = 3.0
    rows = [(10, "10 \u00b5s latency"), (1000, "1 ms latency")]
    for mode, t in THEMES.items():
        c = Canvas(t, 1000, 540)
        c.header("Faster quotes keep more stocks profitable",
                 "Market making at the best bid and ask on the 100 busiest stocks. Profit per $ traded; "
                 "each dot is one stock.")
        ax = c.axes(200, 136, 560, 344)
        ax.set_xlim(-lim - 0.25, lim + 0.25)
        ax.set_ylim(0, 2)
        ax.set_yticks([])
        ax.set_xticks([-3, -2, -1, 0, 1, 2, 3])
        ax.xaxis.set_major_formatter(lambda v, _: f"{'+' if v > 0 else MINUS if v < 0 else ''}{abs(v):g} bp")
        ax.tick_params(axis="x", labelsize=10, pad=6)
        ax.grid(axis="x", color=t["grid"], linewidth=0.8)
        ax.set_axisbelow(True)
        ax.axhline(1, color=t["grid"], linewidth=0.8)
        ax.axvline(0, color=t["axis"], linewidth=1.4, zorder=1)
        ax.text(-0.12, 2.02, r"$\leftarrow$  loses money", ha="right", va="bottom", fontsize=9.5, color=t["muted"])
        ax.text(0.12, 2.02, r"makes money  $\rightarrow$", ha="left", va="bottom", fontsize=9.5, color=t["muted"])
        sx = abs(ax.transData.transform((1, 0))[0] - ax.transData.transform((0, 0))[0])
        sy = abs(ax.transData.transform((0, 1))[1] - ax.transData.transform((0, 0))[1])
        diameter = 7.0
        for row, ((latency, label), color) in enumerate(zip(rows, (t["blue"], t["orange"]))):
            center = 1.5 - row
            pts, wins, runs = data[latency]
            inside = [(sym, v) for sym, v in pts if abs(v) <= lim]
            ys = [center + dy / sy for dy in beeswarm([v * sx for _, v in inside], diameter)]
            ax.scatter([v for _, v in inside], ys, s=(diameter * 0.75) ** 2, color=color,
                       edgecolor=t["surface"], linewidth=0.8, zorder=3)
            # Stocks off the chart: an arrow at the edge, named, with their value.
            for side in (-1, 1):
                off = sorted(((sym, v) for sym, v in pts if v * side > lim), key=lambda p: -abs(p[1]))
                for k, (sym, v) in enumerate(off):
                    y = center + 0.32 - k * 0.2
                    ax.scatter([side * (lim + 0.12)], [y], s=42, marker=">" if side > 0 else "<", color=color,
                               edgecolor=t["surface"], linewidth=0.8, zorder=3, clip_on=False)
                    ax.text(side * (lim + 0.3), y, f"{sym} {'+' if v > 0 else MINUS}{abs(v):.1f}",
                            ha="left" if side > 0 else "right", va="center", fontsize=8.5, color=t["ink2"])
            median = sorted(v for _, v in pts)[len(pts) // 2]
            y0 = 136 + 172 * row
            c.text(36, y0 + 62, label, size=12.5, weight="bold")
            c.text(36, y0 + 85, f"median {'+' if median > 0 else MINUS}{abs(median):.2f} bp", size=10, color="ink2")
            c.text(964, y0 + 60, f"{wins} of {runs}", size=22, weight="bold", ha="right")
            c.text(964, y0 + 92, "made money", size=10, color="ink2", ha="right")
        c.footer()
        c.save(out / f"stocks-{mode}.svg")


# --- animated order book (GIF) ----------------------------------------------------

def capture_fill(day: Path, start=OPEN + 3600 * NS, frame_ns=100_000_000, timeout_s=25, after_s=4,
                 min_ahead=400, min_wait_s=3, depth=10):
    """Finds the first time a 100-share buy joining AAPL's best bid fills while
    the bid stays put, and records the book every `frame_ns` from placement
    until `after_s` after the fill."""
    sim = PassiveSim(day / "AAPL.itch", "AAPL", latency_ns=10_000)
    attempt = None
    for q in sim:
        t = q.timestamp
        fills = sim.take_fills()
        if t < start or not (q.bid and q.ask):
            continue
        if attempt is None:
            if q.ask.price - q.bid.price == TICK:
                sim.place(Side.Buy, q.bid.price, 100)
                attempt = dict(t0=t, price=q.bid.price, frames=[], next=t, fill=None, ahead0=None)
            continue
        ours = sim.working(Side.Buy)
        if attempt["ahead0"] is None and ours is not None:
            attempt["ahead0"] = ours.ahead
        if fills and attempt["fill"] is None:
            attempt["fill"] = (t, fills[-1].price)
        if attempt["fill"] is None and (q.bid.price != attempt["price"] or t - attempt["t0"] > timeout_s * NS):
            sim.place(Side.Buy, attempt["price"], 0)  # give up, cancel, and try again later
            attempt = None
            continue
        while attempt["next"] <= t:
            attempt["frames"].append(dict(
                t=attempt["next"], bids=[(l.price, l.shares) for l in sim.bids(depth)],
                asks=[(l.price, l.shares) for l in sim.asks(depth)], ours=sim.working(Side.Buy),
                mid=(q.bid.price + q.ask.price) / 2, fill=attempt["fill"]))
            attempt["next"] += frame_ns
        if attempt["fill"] and t > attempt["fill"][0] + after_s * NS:
            if (attempt["ahead0"] or 0) >= min_ahead and attempt["fill"][0] - attempt["t0"] >= min_wait_s * NS:
                return attempt
            attempt = None  # filled too quickly to show the queue moving; find another
    raise RuntimeError("no suitable fill found")


def clock(ns: int) -> str:
    s = ns / NS
    return f"{int(s // 3600):02d}:{int(s // 60 % 60):02d}:{s % 60:06.3f}"


def order_book_frame(t: dict, a: dict, f: dict, rows: list[int], xmax: float, mid_range) -> Image.Image:
    c = Canvas(t, 960, 540, dpi=125, page=True)
    ours_price, ahead0 = a["price"], a["ahead0"]
    c.text(36, 30, "AAPL order book, replayed from NASDAQ", size=16, weight="bold")
    c.text(36, 56, "A simulated 100-share buy waits in the real queue at the best bid", size=10.5, color="ink2")
    c.text(924, 28, clock(f["t"]), size=16, weight="bold", ha="right")
    c.text(924, 54, "December 30, 2019", size=10, color="muted", ha="right")

    # Ladder: prices down the middle, bids to the left, asks to the right.
    ax = c.axes(36, 112, 540, 380)
    gutter = xmax * 0.42
    ax.set_xlim(-xmax - gutter, xmax + gutter)
    ax.set_ylim(-0.6, len(rows) - 0.4)
    ax.set_xticks([])
    ax.set_yticks([])
    bids, asks = dict(f["bids"]), dict(f["asks"])
    ours = f["ours"]
    best_bid, best_ask = max(bids, default=None), min(asks, default=None)
    for col, text, ha in ((-gutter - xmax, "BIDS  ·  buyers", "left"), (0, "PRICE", "center"),
                          (gutter + xmax, "sellers  ·  ASKS", "right")):
        ax.text(col, len(rows) - 0.1, text, ha=ha, va="bottom", fontsize=8.5, color=t["muted"], fontweight="bold")
    for i, price in enumerate(rows):
        if price in (best_bid, best_ask):
            ax.add_patch(Rectangle((-xmax - gutter, i - 0.48), 2 * (xmax + gutter), 0.96, facecolor=t["band"],
                                   edgecolor="none", zorder=0))
        ax.text(0, i, dollars(price), ha="center", va="center", fontsize=9.5,
                color=t["ink"] if price in (best_bid, best_ask) else t["ink2"],
                fontweight="bold" if price in (best_bid, best_ask) else "normal")
        h, y = 0.62, i - 0.31
        if price in asks:
            size = asks[price]
            rounded_bar(ax, gutter / 2, y, min(size, xmax), h, t["orange"], square="left", radius=3)
            ax.text(gutter / 2 + min(size, xmax) + xmax * 0.02, i, f"{size:,}", va="center", fontsize=8,
                    color=t["muted"])
        if price in bids:
            size = bids[price]
            end = -gutter / 2 - min(size + (ours.shares if ours and ours.price == price else 0), xmax)
            if ours is not None and price == ours.price:
                ahead = min(ours.ahead, size)
                segments = [(ahead, t["blue"]), (ours.shares, t["aqua"]), (size - ahead, t["blue"])]
                left = -gutter / 2
                for k, (width, color) in enumerate(segments):
                    if width > 0:
                        rounded_bar(ax, left, y, -width, h, color, radius=3,
                                    square=None if k == len(segments) - 1 or (k == 1 and size - ahead <= 0)
                                    else "left", gap=t["surface"])
                    if k == 1:
                        ax.text(left - width / 2, i + 0.42, "YOU", ha="center", va="bottom", fontsize=8.5,
                                fontweight="bold", color=t["ink"])
                    left -= width
            else:
                rounded_bar(ax, -gutter / 2, y, -min(size, xmax), h, t["blue"], square="right", radius=3)
            ax.text(end - xmax * 0.02, i, f"{size:,}", va="center", ha="right", fontsize=8, color=t["muted"])

    # Side panel: status, the big number, and the price since we joined.
    x0 = 612
    filled = f["fill"] is not None
    c.text(x0, 112, "●", size=11, color=t["aqua"])
    c.text(x0 + 18, 113, "FILLED" if filled else "WAITING IN THE QUEUE", size=10, weight="bold", color="ink2")
    c.text(x0, 136, f"Buy 100 @ {dollars(ours_price)}", size=13, weight="bold")
    if not filled:
        ahead = ours.ahead if ours is not None else ahead0
        c.text(x0, 168, f"{ahead:,}", size=40, weight="bold")
        c.text(x0, 222, "shares ahead of you", size=10.5, color="ink2")
        bar = c.axes(x0, 248, 312, 8)
        bar.set_xlim(0, 1)
        bar.set_ylim(0, 1)
        bar.set_xticks([])
        bar.set_yticks([])
        rounded_bar(bar, 0, 0, 1, 1, t["grid"], radius=4)
        rounded_bar(bar, 0, 0, max(0.02, 1 - ahead / ahead0), 1, t["aqua"], radius=4)
        c.text(x0, 264, f"{ahead0 - ahead:,} of {ahead0:,} cleared", size=9.5, color="muted")
    else:
        fill_t, fill_price = f["fill"]
        mark = (f["mid"] - fill_price) / 100
        c.text(x0, 168, cents(mark, 1), size=40, weight="bold")
        c.text(x0, 222, "per share, marked to the mid price", size=10.5, color="ink2")
        c.text(x0, 248, f"Filled at {clock(fill_t)}.\nThe price then fell through your level:\n"
               "adverse selection.", size=9.5, color="muted", linespacing=1.45)

    spark = c.axes(x0, 352, 312, 112)
    times = [g["t"] for g in a["frames"]]
    seen = [g for g in a["frames"] if g["t"] <= f["t"]]
    spark.set_xlim(times[0], times[-1])
    spark.set_ylim(*mid_range)
    spark.set_xticks([])
    spark.set_yticks([])
    spark.axhline(ours_price, color=t["aqua"], linewidth=1.2, linestyle=(0, (3, 3)))
    spark.text(times[-1], ours_price, " your price", fontsize=8.5, color=t["ink2"], va="bottom", ha="right")
    spark.plot([g["t"] for g in seen], [g["mid"] for g in seen], color=t["ink"], linewidth=2,
               solid_capstyle="round")
    if filled:
        ft = f["fill"][0]
        spark.axvline(ft, color=t["muted"], linewidth=1, linestyle=(0, (2, 2)))
        spark.text(ft, mid_range[1], " fill", fontsize=8.5, color=t["ink2"], va="top")
    c.text(x0, 340, "Mid price since you joined", size=9.5, color="muted", va="bottom")
    c.text(36, 516, "Your order joins the back of the queue and moves up as orders ahead trade or cancel. "
           "Simulated with 10 µs latency.", size=8.5, color="muted", va="bottom")
    return c.frame()


def order_book_gif(day: Path, out: Path):
    a = capture_fill(day)
    rows = list(range(a["price"] - 6 * TICK, a["price"] + 7 * TICK, TICK))
    visible = [s for f in a["frames"] for p, s in f["bids"] + f["asks"] if p in rows]
    xmax = sorted(visible)[int(0.95 * (len(visible) - 1))] * 1.1
    mids = [f["mid"] for f in a["frames"]] + [a["price"]]
    pad = (max(mids) - min(mids)) * 0.25 or TICK
    mid_range = (min(mids) - pad, max(mids) + pad)
    for mode, t in THEMES.items():
        images = [order_book_frame(t, a, f, rows, xmax, mid_range) for f in a["frames"]]
        palette = images[-1].quantize(colors=96, method=Image.Quantize.MEDIANCUT)
        frames = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in images]
        durations = [70] * (len(frames) - 1) + [3000]  # hold the last frame
        frames[0].save(out / f"order-book-{mode}.gif", save_all=True, append_images=frames[1:],
                       duration=durations, loop=0, optimize=True)
    return a


# --- animated README banner (SVG with CSS animation, which GitHub lets play) -------

LOOP_S = 13  # one pass of the replay, including the hold at the end
TAPE = ["268.7M messages replayed", "8,892 stocks rebuilt", "0 book errors", "54.7M messages per second",
        "99.81% of fills matched NASDAQ's queue", "400 backtests across 100 stocks",
        "39 of 100 stocks profitable at 10 µs", "16 of 100 at 1 ms"]
CHIPS = ["NASDAQ ITCH", "C++20", "Python", "pybind11", "GoogleTest"]


def keyframes(name: str, stops: list[tuple[float, str]]) -> str:
    """Keeps each value until just before it changes, so the bars step like a
    real book but slide rather than jump."""
    out = [f"0%{{{stops[0][1]}}}"]
    last = stops[0][1]
    for pct, value in stops[1:]:
        if value != last:
            out += [f"{max(pct - 0.5, 0.1):.2f}%{{{last}}}", f"{pct:.2f}%{{{value}}}"]
            last = value
    out.append(f"100%{{{last}}}")
    return f"@keyframes {name}{{{''.join(out)}}}"


def banner_svg(t: dict, a: dict, rows: list[int], xmax: float) -> str:
    frames = a["frames"][::2] + [a["frames"][-1]]
    pct = [85 * i / (len(frames) - 1) for i in range(len(frames))]
    scale_w, row_h, top = 186, 19.5, 44
    bid_edge, ask_edge, price_x = 962, 1046, 1004
    css, shapes = [], []

    def bar(name, side, price, y):
        book = "bids" if side == "bid" else "asks"
        stops = [(p, f"transform:scaleX({min(dict(f[book]).get(price, 0) / xmax, 1):.3f})")
                 for p, f in zip(pct, frames)]
        if all(s.endswith("(0.000)") for _, s in stops):
            return
        css.append(keyframes(name, stops))
        x = bid_edge - scale_w if side == "bid" else ask_edge
        origin = "right" if side == "bid" else "left"
        color = t["blue"] if side == "bid" else t["orange"]
        shapes.append(f'<rect x="{x}" y="{y}" width="{scale_w}" height="13" rx="2" fill="{color}" '
                      f'style="{stops[0][1]};transform-origin:{origin} center;transform-box:fill-box;'
                      f'animation:{name} {LOOP_S}s linear infinite"/>')

    for i, price in enumerate(reversed(rows)):
        y = top + i * row_h
        bar(f"b{i}", "bid", price, y)
        bar(f"a{i}", "ask", price, y)
        weight = "700" if price == a["price"] else "400"
        fill = t["ink"] if price == a["price"] else t["ink2"]
        shapes.append(f'<text x="{price_x}" y="{y + 11}" text-anchor="middle" class="px" fill="{fill}" '
                      f'font-weight="{weight}">{dollars(price)}</text>')

    # Our order: a block just behind the shares ahead of it, gone once filled.
    width = 100 / xmax * scale_w
    y = top + list(reversed(rows)).index(a["price"]) * row_h
    move = [(p, f"transform:translateX({-min(f['ours'].ahead / xmax, 1) * scale_w:.1f}px);opacity:1"
             if f["ours"] is not None else "") for p, f in zip(pct, frames)]
    first = next(v for _, v in move if v)
    gone = next(p for (p, v), f in zip(move, frames) if f["fill"] is not None)
    stops, held = [], first
    for p, v in move:
        held = v or held
        stops.append((p, held if p < gone else held.replace("opacity:1", "opacity:0")))
    css.append(keyframes("you", stops))
    shapes.append(f'<g style="{first};animation:you {LOOP_S}s linear infinite">'
                  f'<rect x="{bid_edge - width:.1f}" y="{y}" width="{width:.1f}" height="13" rx="2" fill="{t["aqua"]}"/>'
                  f'<text x="{bid_edge - width - 6:.1f}" y="{y + 11}" text-anchor="end" class="you" fill="{t["ink"]}">YOU</text></g>')

    # Status line above the ladder: waiting, then filled.
    fill_cut = f"{gone:.2f}%"
    css.append(f"@keyframes wait{{0%,{gone - 0.5:.2f}%{{opacity:1}}{fill_cut},100%{{opacity:0}}}}")
    css.append(f"@keyframes done{{0%,{gone - 0.5:.2f}%{{opacity:0}}{fill_cut},100%{{opacity:1}}}}")
    status = (f'<text x="790" y="30" class="status" fill="{t["ink2"]}" style="animation:wait {LOOP_S}s linear infinite">'
              f'AAPL, Dec 30 2019  ·  {a["ahead0"]} shares ahead of you</text>'
              f'<text x="790" y="30" class="status" fill="{t["ink2"]}" style="opacity:0;animation:done {LOOP_S}s linear infinite">'
              f'AAPL, Dec 30 2019  ·  Filled, then the price fell through you</text>')

    chips, x = [], 56
    for chip in CHIPS:
        w = len(chip) * 8 + 30
        chips.append(f'<rect x="{x}" y="248" width="{w}" height="30" rx="15" fill="none" stroke="{t["axis"]}"/>'
                     f'<text x="{x + w / 2}" y="268" text-anchor="middle" class="chip">{chip}</text>')
        x += w + 10

    line = "    •    ".join(TAPE) + "    •    "
    copy_w = round(len(line) * 7.9)
    tape = "".join(f'<text x="{i * copy_w}" y="339" class="tape" textLength="{copy_w}" lengthAdjust="spacing" '
                   f'xml:space="preserve">{line}</text>' for i in range(3))
    alt = ("market-data-pipeline: every order from a real NASDAQ trading day, rebuilt, queued and backtested "
           "in C++20 and Python")
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="360" viewBox="0 0 1280 360" role="img" aria-label="{alt}">
<style>
  text {{ font-family: system-ui, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; }}
  .eyebrow {{ font-size: 14px; font-weight: 600; letter-spacing: 2.4px; fill: {t["muted"]}; }}
  .title {{ font-size: 62px; font-weight: 700; letter-spacing: -1.5px; fill: {t["ink"]}; }}
  .tag {{ font-size: 22px; fill: {t["ink2"]}; }}
  .chip {{ font-size: 14px; font-weight: 500; fill: {t["ink2"]}; }}
  .tape {{ font-size: 15px; font-weight: 500; fill: {t["ink2"]}; font-variant-numeric: tabular-nums; }}
  .px {{ font-size: 11.5px; font-variant-numeric: tabular-nums; }}
  .you {{ font-size: 10px; font-weight: 700; }}
  .status {{ font-size: 12.5px; font-weight: 600; }}
  .scroll {{ animation: scroll {copy_w / 34:.0f}s linear infinite; }}
  @keyframes scroll {{ to {{ transform: translateX(-{copy_w}px); }} }}
  {"".join(css)}
  @media (prefers-reduced-motion: reduce) {{ * {{ animation: none !important; }} }}
</style>
<defs><clipPath id="card"><rect width="1280" height="360" rx="16"/></clipPath></defs>
<g clip-path="url(#card)">
  <rect width="1280" height="360" fill="{t["surface"]}"/>
  {status}
  {"".join(shapes)}
  <text x="56" y="84" class="eyebrow">NASDAQ ITCH 5.0  ·  C++20  ·  PYTHON</text>
  <text x="52" y="156" class="title">market-data-pipeline</text>
  <text x="56" y="203" class="tag">Every order from a real NASDAQ trading day,</text>
  <text x="56" y="231" class="tag">rebuilt, queued and backtested from raw data.</text>
  {"".join(chips)}
  <rect y="306" width="1280" height="54" fill="{t["band"]}"/>
  <g class="scroll">{tape}</g>
</g>
<rect x="0.5" y="0.5" width="1279" height="359" rx="16" fill="none" stroke="{t["border"]}"/>
</svg>
"""


def banner(a: dict, out: Path):
    rows = list(range(a["price"] - 6 * TICK, a["price"] + 7 * TICK, TICK))
    visible = [s for f in a["frames"] for p, s in f["bids"] + f["asks"] if p in rows]
    xmax = sorted(visible)[int(0.95 * (len(visible) - 1))] * 1.1
    for mode, t in THEMES.items():
        (out / f"banner-{mode}.svg").write_text(banner_svg(t, a, rows, xmax))


def main(day: str, out: str) -> int:
    day_dir, out_dir = Path(day).expanduser(), Path(out)
    out_dir.mkdir(parents=True, exist_ok=True)
    queue_audit(out_dir)
    imbalance(day_dir, out_dir)
    waterfall(aapl_market_making(day_dir), out_dir)
    stocks(day_dir, out_dir)
    a = order_book_gif(day_dir, out_dir)
    banner(a, out_dir)
    print(f"order book: placed at {clock(a['t0'])}, {len(a['frames'])} frames, "
          f"{a['ahead0']} shares ahead at the start")
    for p in sorted(out_dir.iterdir()):
        print(f"  {p.name:28} {p.stat().st_size / 1024:8,.0f} KB")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <split-day-dir> <out-dir>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
