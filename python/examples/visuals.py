"""Makes the README's images from real results, in light and dark variants.

    PYTHONPATH=build-release:python python3 python/examples/visuals.py ~/data/itch/12302019 docs/img

Reads the per-symbol files from itch_split and results.jsonl from runner.py
in that folder. Needs matplotlib and Pillow (not needed for anything else).
"""

import json
import logging
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from PIL import Image  # noqa: E402

from backtest import NS, OPEN, next_move_by_imbalance  # noqa: E402
from itchpy import PassiveSim, QuoteStream, Side  # noqa: E402
from market_making import JoinBest, make_markets  # noqa: E402

logging.getLogger("matplotlib.font_manager").setLevel(logging.ERROR)

# Reference palette: surfaces, ink, and categorical/diverging steps per mode,
# checked with the dataviz palette validator (all pass; light aqua needs a label).
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781",
                  grid="#e1e0d9", axis="#c3c2b7", blue="#2a78d6", orange="#eb6834",
                  aqua="#1baf7a", red="#e34948", blue_soft="#86b6ef", orange_soft="#f2a07d"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781",
                 grid="#2c2c2a", axis="#383835", blue="#3987e5", orange="#d95926",
                 aqua="#199e70", red="#e66767", blue_soft="#1c5cab", orange_soft="#8f3a18"),
}
FONT = ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"]
TICK = 100  # $0.01 in price units
MINUS = "−"


def cents(x: float, digits: int = 2) -> str:
    return f"{'+' if x >= 0 else MINUS}{abs(x):.{digits}f}¢"


def dollars(price: int) -> str:
    return f"${price / 10_000:,.2f}"


def setup(theme: dict, size: tuple[float, float]):
    plt.rcParams.update({
        "font.family": "sans-serif", "font.sans-serif": FONT, "font.size": 11,
        "text.color": theme["ink"], "axes.labelcolor": theme["ink2"],
        "xtick.color": theme["muted"], "ytick.color": theme["muted"],
        "axes.edgecolor": theme["axis"], "axes.facecolor": theme["surface"],
        "figure.facecolor": theme["surface"], "savefig.facecolor": theme["surface"],
    })
    fig, ax = plt.subplots(figsize=size, dpi=100)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.tick_params(length=0)
    ax.grid(axis="y", color=theme["grid"], linewidth=0.8)
    ax.set_axisbelow(True)
    return fig, ax


def titles(fig, theme, title: str, subtitle: str):
    fig.text(0.04, 0.95, title, fontsize=16, fontweight="bold", color=theme["ink"], va="top")
    fig.text(0.04, 0.885, subtitle, fontsize=11, color=theme["ink2"], va="top")


def save(fig, out: Path, name: str, mode: str):
    fig.savefig(out / f"{name}-{mode}.png", dpi=100, facecolor=fig.get_facecolor())
    plt.close(fig)


# --- data -------------------------------------------------------------------

def aapl_market_making(day: Path):
    sim = PassiveSim(day / "AAPL.itch", "AAPL", latency_ns=10_000)
    return make_markets(sim, JoinBest(size=100, max_position=500), rebate_per_share=20)


def opening_price(path: Path, symbol: str) -> float:
    for q in QuoteStream(path, symbol):
        if q.timestamp >= OPEN and q.bid and q.ask:
            return (q.bid.price + q.ask.price) / 2 / 10_000
    return float("nan")


# --- charts -----------------------------------------------------------------

def waterfall(r, out: Path):
    n = r.shares_filled
    spread = 100 * r.spread_captured / n
    adverse = 100 * r.markout_1s / n
    rebates = 100 * r.rebates / n
    net = 100 * r.pnl / n
    later = net - spread - adverse - rebates  # moves after 1 s and the 16:00 close-out
    steps = [("Spread\ncaptured", spread), ("Adverse selection\n(first second)", adverse),
             ("Later moves and\n16:00 close-out", later), ("NASDAQ\nrebate", rebates)]

    for mode, t in THEMES.items():
        fig, ax = setup(t, (10, 5.2))
        fig.subplots_adjust(left=0.08, right=0.96, top=0.76, bottom=0.17)
        level = 0.0
        for i, (label, value) in enumerate(steps):
            color = t["blue"] if value >= 0 else t["red"]
            ax.bar(i, value, bottom=level, width=0.56, color=color, edgecolor=t["surface"], linewidth=2)
            top = max(level, level + value)
            ax.text(i, top + 0.06, cents(value), ha="center", va="bottom", fontsize=12,
                    fontweight="bold", color=t["ink"])
            if i < len(steps) - 1:  # connector to the next step
                ax.plot([i + 0.28, i + 0.72], [level + value] * 2, color=t["muted"], linewidth=1, linestyle=":")
            level += value
        ax.bar(len(steps), net, width=0.56, color=t["ink2"], edgecolor=t["surface"], linewidth=2)
        ax.text(len(steps), 0.06, cents(net), ha="center", va="bottom", fontsize=12, fontweight="bold", color=t["ink"])
        ax.axhline(0, color=t["axis"], linewidth=1.2)
        ax.axhline(-1.36, color=t["muted"], linewidth=1, linestyle="--")
        ax.text(len(steps) + 0.42, -1.30, f"market orders\n{cents(-1.36)}", va="bottom", ha="left",
                fontsize=10, color=t["ink2"])
        ax.set_xticks(range(len(steps) + 1), [s for s, _ in steps] + ["Net per\nshare"])
        ax.tick_params(axis="x", colors=t["ink2"], labelsize=10.5)
        ax.yaxis.set_major_formatter(lambda v, _: cents(v, 1) if v else "0")
        ax.set_xlim(-0.6, len(steps) + 1.3)
        ax.set_ylim(min(-2.2, level - 0.3), 1.5)
        titles(fig, t, "Resting orders earn the spread, then lose it to adverse selection",
               f"AAPL market making, 2019-12-30: join the best bid and ask, 10 µs latency. "
               f"Per share filled ({n:,} shares).")
        save(fig, out, "pnl-waterfall", mode)


def imbalance(day: Path, out: Path):
    quotes = list(QuoteStream(day / "AAPL.itch", "AAPL"))
    rows = next_move_by_imbalance(quotes)
    labels = [f"{lo:.1f}–{hi:.1f}" for lo, hi, _, _ in rows]
    shares = [100 * up / (up + down) for _, _, up, down in rows]

    for mode, t in THEMES.items():
        fig, ax = setup(t, (10, 5.2))
        fig.subplots_adjust(left=0.08, right=0.96, top=0.76, bottom=0.15)
        ax.bar(range(len(rows)), shares, width=0.56, color=t["blue"], edgecolor=t["surface"], linewidth=2)
        for i, s in enumerate(shares):
            ax.text(i, s + 1, f"{s:.1f}%", ha="center", va="bottom", fontsize=12, fontweight="bold", color=t["ink"])
        ax.axhline(50, color=t["muted"], linewidth=1, linestyle="--")
        ax.text(-0.45, 51, "coin flip", ha="left", va="bottom", fontsize=10, color=t["ink2"])
        ax.set_xticks(range(len(rows)), labels)
        ax.tick_params(axis="x", colors=t["ink2"], labelsize=11)
        ax.set_xlabel("Top-of-book imbalance: bid shares ÷ (bid + ask shares)", fontsize=11, labelpad=10)
        ax.yaxis.set_major_formatter(lambda v, _: f"{v:.0f}%")
        ax.set_ylim(0, 75)
        titles(fig, t, "Order book imbalance predicts AAPL's next price move",
               "Share of quotes where the next mid-price change is up, 09:30–16:00, 2019-12-30.")
        save(fig, out, "imbalance", mode)


def queue_audit(out: Path):
    # From the full-day queue audit (tools/itch_queue) over all 8,892 stocks.
    for mode, t in THEMES.items():
        fig = plt.figure(figsize=(10, 3.2), dpi=100)
        fig.patch.set_facecolor(t["surface"])
        fig.text(0.05, 0.86, "Does our matching engine queue orders like NASDAQ?", fontsize=15,
                 fontweight="bold", color=t["ink"], va="top")
        fig.text(0.05, 0.62, "99.81%", fontsize=58, fontweight="bold", color=t["ink"], va="top")
        fig.text(0.43, 0.56, "of 5,683,179 regular-hours executions hit the order\n"
                 "our engine had at the front of the queue", fontsize=13, color=t["ink2"], va="top",
                 linespacing=1.4)
        fig.text(0.05, 0.13, "0 at a worse price   ·   0 unknown orders   ·   0 book errors   ·   "
                 "all 8,892 stocks, 2019-12-30", fontsize=11, color=t["muted"], va="bottom")
        save(fig, out, "queue-audit", mode)


def stocks(day: Path, out: Path):
    records = [json.loads(line) for line in (day / "results.jsonl").read_text().splitlines()]
    latest = {(r["symbol"], r["strategy"], r["latency_us"]): r for r in records if "error" not in r}
    prices = {s: opening_price(day / f"{s}.itch", s) for s, _, _ in latest}
    series = []
    for latency in (10, 1000):
        # Profit per dollar traded, in basis points: cents per share would make
        # a $1,800 stock look 100 times riskier than an $18 one.
        pts = [(prices[s], 10_000 * r["pnl"] / r["shares"] / prices[s], s)
               for (s, strat, lat), r in latest.items() if strat == "join" and lat == latency and r["shares"]]
        series.append((latency, pts))
    callouts = {"AMZN", "TSLA", "GOOGL", "AAPL", "NIO"}

    for mode, t in THEMES.items():
        fig, ax = setup(t, (10, 5.6))
        fig.subplots_adjust(left=0.09, right=0.96, top=0.79, bottom=0.13)
        ax.grid(axis="x", color=t["grid"], linewidth=0.8)
        ax.axhline(0, color=t["axis"], linewidth=1.2)
        for (latency, pts), color, label in zip(series, (t["blue"], t["orange"]), ("10 µs latency", "1 ms latency")):
            ax.scatter([p for p, _, _ in pts], [c for _, c, _ in pts], s=42, color=color,
                       edgecolor=t["surface"], linewidth=1.5, label=label, zorder=3)
        for price, pnl, sym in series[0][1]:
            if sym in callouts:
                ax.annotate(sym, (price, pnl), xytext=(7, -3 if pnl < 0 else 3), textcoords="offset points",
                            fontsize=10, color=t["ink2"], fontweight="bold")
        ax.set_xscale("log")
        ax.xaxis.set_major_formatter(lambda v, _: f"${v:,.0f}")
        ax.yaxis.set_major_formatter(lambda v, _: f"{'+' if v > 0 else MINUS if v < 0 else ''}{abs(v):g} bp")
        ax.set_xlabel("Share price at the open (log scale)", fontsize=11, labelpad=8)
        ax.set_ylabel("Profit per $ traded (basis points)", fontsize=11)
        legend = ax.legend(loc="lower left", frameon=False, fontsize=10.5, labelcolor=t["ink2"])
        legend.set_zorder(4)
        n10 = sum(1 for _, c, _ in series[0][1] if c > 0)
        n1000 = sum(1 for _, c, _ in series[1][1] if c > 0)
        titles(fig, t, "Market making across the 100 busiest stocks",
               f"Join the best bid and ask, 2019-12-30. Profitable: {n10} of 100 at 10 µs, "
               f"{n1000} of 100 at 1 ms. Each dot is one stock.")
        save(fig, out, "stocks", mode)


# --- animated order book -------------------------------------------------------

def capture_fill(day: Path, start=OPEN + 3600 * NS, frame_ns=150_000_000, timeout_s=25, after_s=4,
                 min_ahead=400, min_wait_s=3, depth=10):
    """Finds the first time a 100-share buy joining AAPL's best bid fills while
    the bid stays put, and records the book every `frame_ns` from placement
    until `after_s` after the fill."""
    sim = PassiveSim(day / "AAPL.itch", "AAPL", latency_ns=10_000)
    attempt = None

    def snapshot(t):
        return dict(t=t, bids=[(l.price, l.shares) for l in sim.bids(depth)],
                    asks=[(l.price, l.shares) for l in sim.asks(depth)], ours=sim.working(Side.Buy))

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
            sim.place(Side.Buy, attempt["price"], 0)  # give up, cancel and try again later
            attempt = None
            continue

        while attempt["next"] <= t:
            frame = snapshot(attempt["next"])
            frame["fill"] = attempt["fill"]
            attempt["frames"].append(frame)
            attempt["next"] += frame_ns
        if attempt["fill"] and t > attempt["fill"][0] + after_s * NS:
            waited = attempt["fill"][0] - attempt["t0"]
            if (attempt["ahead0"] or 0) >= min_ahead and waited >= min_wait_s * NS:
                return attempt
            attempt = None  # filled too quickly to show the queue moving; find another
    raise RuntimeError("no suitable fill found")


def order_book_gif(day: Path, out: Path):
    a = capture_fill(day)
    ours_price = a["price"]
    rows = list(range(ours_price - 6 * TICK, ours_price + 7 * TICK, TICK))
    visible = [s for f in a["frames"] for p, s in f["bids"] + f["asks"] if p in rows]
    xmax = sorted(visible)[int(0.97 * (len(visible) - 1))] * 1.15

    for mode, t in THEMES.items():
        setup(t, (1, 1))
        plt.close("all")
        images = []
        for f in a["frames"]:
            fig = plt.figure(figsize=(9.6, 5.4), dpi=100)
            ax = fig.add_axes([0.1, 0.1, 0.5, 0.7])
            for side in ("top", "right", "left", "bottom"):
                ax.spines[side].set_visible(False)
            ax.tick_params(length=0)
            ax.set_facecolor(t["surface"])
            bids, asks = dict(f["bids"]), dict(f["asks"])
            ours = f["ours"]
            for price in rows:
                y = (price - rows[0]) / TICK
                if price in asks:
                    ax.barh(y, asks[price], height=0.72, color=t["orange"], edgecolor=t["surface"], linewidth=2)
                if price in bids:
                    total = bids[price]
                    if ours is not None and price == ours.price:
                        ahead = min(ours.ahead, total)
                        parts = [(0, ahead, t["blue"]), (ahead, ours.shares, t["aqua"]),
                                 (ahead + ours.shares, total - ahead, t["blue"])]
                        for left, width, color in parts:
                            if width > 0:
                                ax.barh(y, -width, left=-left, height=0.72, color=color,
                                        edgecolor=t["surface"], linewidth=2)
                        ax.annotate("YOU", (-(ahead + ours.shares / 2), y + 0.36), xytext=(0, 8),
                                    textcoords="offset points", ha="center", va="bottom", fontsize=10,
                                    fontweight="bold", color=t["ink"],
                                    arrowprops=dict(arrowstyle="-", color=t["ink2"], linewidth=1))
                    else:
                        ax.barh(y, -total, height=0.72, color=t["blue"], edgecolor=t["surface"], linewidth=2)
            ax.axvline(0, color=t["axis"], linewidth=1.2)
            ax.set_xlim(-xmax, xmax)
            ax.set_ylim(-0.6, len(rows) - 0.4)
            ax.set_yticks(range(len(rows)), [dollars(p) for p in rows], fontsize=9.5, color=t["ink2"])
            ax.xaxis.set_major_formatter(lambda v, _: f"{abs(v):,.0f}")
            ax.tick_params(axis="x", colors=t["muted"], labelsize=9)
            ax.set_title("BIDS (buyers)", loc="left", fontsize=10.5, fontweight="bold", color=t["ink2"])
            ax.set_title("ASKS (sellers)", loc="right", fontsize=10.5, fontweight="bold", color=t["ink2"])
            ax.set_xlabel("Shares resting at each price", fontsize=10, color=t["ink2"])

            secs = f["t"] / NS
            clock = f"{int(secs // 3600):02d}:{int(secs // 60 % 60):02d}:{secs % 60:06.3f}"
            fig.text(0.04, 0.95, "AAPL order book, replayed from NASDAQ ITCH", fontsize=15,
                     fontweight="bold", color=t["ink"], va="top")
            fig.text(0.04, 0.885, f"December 30, 2019   {clock}", fontsize=11, color=t["ink2"], va="top")

            px = 0.66
            fig.text(px, 0.78, f"Your order: buy 100 @ {dollars(ours_price)}", fontsize=12,
                     fontweight="bold", color=t["ink"])
            if f["fill"] is None:
                ahead = ours.ahead if ours is not None else (a["ahead0"] or 0)
                fig.text(px, 0.68, "Shares ahead of you", fontsize=11, color=t["ink2"])
                fig.text(px, 0.56, f"{ahead:,}", fontsize=40, fontweight="bold", color=t["ink"])
                fig.text(px, 0.46, f"started at {a['ahead0']:,}", fontsize=10.5, color=t["muted"])
                note = ("You joined the back of the queue.\nOrders ahead that trade or cancel\n"
                        "move you forward.")
            else:
                fill_t, fill_price = f["fill"]
                mid = (bids and asks) and (max(bids) + min(asks)) / 2
                mark = (mid - fill_price) / 100 if mid else 0.0
                fs = fill_t / NS
                fig.text(px, 0.68, f"Filled at {int(fs // 3600):02d}:{int(fs // 60 % 60):02d}:{fs % 60:06.3f}",
                         fontsize=11, color=t["ink2"])
                fig.text(px, 0.56, cents(mark, 1), fontsize=40, fontweight="bold", color=t["ink"])
                fig.text(px, 0.46, "per share, marked to the mid price", fontsize=10.5, color=t["muted"])
                note = ("Filled. Now watch the price:\nfills often come just before\n"
                        "it moves against you.")
            fig.text(px, 0.18, note, fontsize=10.5, color=t["ink2"], linespacing=1.5)
            fig.text(px, 0.08, "Simulated with 10 µs latency", fontsize=9, color=t["muted"])

            fig.canvas.draw()
            images.append(Image.frombuffer("RGBA", fig.canvas.get_width_height(),
                                           fig.canvas.buffer_rgba()).convert("RGB"))
            plt.close(fig)

        frames = [im.quantize(colors=64, method=Image.Quantize.MEDIANCUT) for im in images]
        durations = [100] * (len(frames) - 1) + [2500]  # hold the last frame
        frames[0].save(out / f"order-book-{mode}.gif", save_all=True, append_images=frames[1:],
                       duration=durations, loop=0, optimize=True)
    return a


def main(day: str, out: str) -> int:
    day_dir, out_dir = Path(day).expanduser(), Path(out)
    out_dir.mkdir(parents=True, exist_ok=True)
    r = aapl_market_making(day_dir)
    waterfall(r, out_dir)
    imbalance(day_dir, out_dir)
    queue_audit(out_dir)
    stocks(day_dir, out_dir)
    a = order_book_gif(day_dir, out_dir)
    secs = a["t0"] / NS
    print(f"order book: placed at {int(secs // 3600):02d}:{int(secs // 60 % 60):02d}:{secs % 60:06.3f}, "
          f"{len(a['frames'])} frames, {a['ahead0']} shares ahead at the start")
    for p in sorted(out_dir.iterdir()):
        print(f"  {p.name:28} {p.stat().st_size / 1024:8,.0f} KB")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <split-day-dir> <out-dir>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
