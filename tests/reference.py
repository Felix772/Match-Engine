"""Independent price/FIFO oracle for both CLIs; only Python's standard library."""
import collections
import pathlib
import random
import subprocess
import sys
import tempfile


def reference(lines):
    books = {"B": {}, "S": {}}
    index = {}
    trades = []
    for line in lines:
        fields = line.split(",")
        if fields[0] == "C":
            oid = int(fields[2])
            if oid in index:
                side, price = index.pop(oid)
                del books[side][price][oid]
                if not books[side][price]:
                    del books[side][price]
            continue
        _, ts, oid, side, price, qty, trader = fields
        ts, oid, price, qty = map(int, (ts, oid, price, qty))
        opposite = books["S" if side == "B" else "B"]
        while qty and opposite:
            best = min(opposite) if side == "B" else max(opposite)
            if (side == "B" and best > price) or (side == "S" and best < price):
                break
            level = opposite[best]
            resting_id = next(iter(level))
            resting = level[resting_id]
            size = min(qty, resting[0])
            qty -= size
            resting[0] -= size
            buy, sell, buyer, seller = ((oid, resting_id, trader, resting[1]) if side == "B"
                                       else (resting_id, oid, resting[1], trader))
            trades.append(f"T,{ts},{best},{size},{buy},{sell},{buyer},{seller}")
            if not resting[0]:
                del level[resting_id]
                del index[resting_id]
            if not level:
                del opposite[best]
        if qty:
            books[side].setdefault(price, collections.OrderedDict())[oid] = [qty, trader]
            index[oid] = (side, price)
    return trades


def verify(executable, path):
    expected = reference(path.read_text().splitlines())
    for mode in ("--csv", "--pipeline"):
        result = subprocess.run([executable, mode, str(path)], capture_output=True,
                                text=True, check=True, timeout=60)
        assert result.stdout.splitlines() == expected, (mode, path)
    print(f"{path.name}: both modes match {len(expected)} reference trades")


if __name__ == "__main__":
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    verify(executable, pathlib.Path(__file__).resolve().parents[1] / "data.csv")
    rng = random.Random(772)
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "random.csv"
        for run in range(5):
            events = []
            for i in range(20000):
                if i and rng.random() < .3:
                    events.append(f"C,{i},{rng.randrange(i)}")
                else:
                    events.append(f"A,{i},{i},{rng.choice('BS')},{rng.randrange(1,2000)},"
                                  f"{rng.randrange(1,100)},t{rng.randrange(10)}")
            path.write_text("\n".join(events))
            verify(executable, path)
