"""BFS map layout + pathfinding.

layout(data, start_map, start_room) -> { (map, room) -> (cell_x, cell_y) }
    BFS-walks exits to lay out rooms on a 2D grid using direction vectors.
    U/D are NOT expanded on the 2D grid (they become markers only).
    Rooms on other maps are skipped (layout is per-map).

pathfind(data, (m1, r1), (m2, r2)) -> list[Exit] | None
    BFS shortest path between two rooms, honoring U/D and cross-map exits.
    Returns the list of Exit objects to traverse.
"""
from __future__ import annotations
from collections import deque
from mudmap_exit import Exit, parse_room_exits, REVERSE_DIR

DIR_VEC = {
    "N":  ( 0, -1),
    "S":  ( 0,  1),
    "E":  ( 1,  0),
    "W":  (-1,  0),
    "NE": ( 1, -1),
    "NW": (-1, -1),
    "SE": ( 1,  1),
    "SW": (-1,  1),
}


def layout(data: dict, start_map: int, start_room: int, grid: int = 50) -> dict:
    """BFS outward from start room. Returns {(map, room) -> (cx, cy)}."""
    rooms = data["rooms"]
    if (start_map, start_room) not in rooms:
        return {}
    center = grid // 2
    pos: dict[tuple[int, int], tuple[int, int]] = {(start_map, start_room): (center, center)}
    used_cells: set[tuple[int, int]] = {(center, center)}
    q = deque([(start_map, start_room)])
    while q:
        m, r = q.popleft()
        row = rooms.get((m, r))
        if not row:
            continue
        cx, cy = pos[(m, r)]
        for ex in parse_room_exits(row):
            if ex.dir not in DIR_VEC:
                continue
            # Skip cross-map exits for layout (they'd pull in another map's rooms)
            if ex.map != start_map:
                continue
            key = (ex.map, ex.room)
            if key in pos:
                continue
            dx, dy = DIR_VEC[ex.dir]
            nx, ny = cx + dx, cy + dy
            if nx < 0 or nx >= grid or ny < 0 or ny >= grid:
                continue
            if (nx, ny) in used_cells:
                # Collision: try nearby cells to avoid overwriting
                placed = False
                for r2 in range(1, 4):
                    for ox in range(-r2, r2 + 1):
                        for oy in range(-r2, r2 + 1):
                            tx, ty = nx + ox, ny + oy
                            if 0 <= tx < grid and 0 <= ty < grid and (tx, ty) not in used_cells:
                                nx, ny = tx, ty
                                placed = True
                                break
                        if placed:
                            break
                    if placed:
                        break
                if not placed:
                    continue
            pos[key] = (nx, ny)
            used_cells.add((nx, ny))
            q.append(key)
    return pos


def pathfind(
    data: dict,
    src: tuple[int, int],
    dst: tuple[int, int],
    avoid_restricted: bool = False,
) -> list[Exit] | None:
    """BFS shortest path as list of Exit objects. None if unreachable."""
    if src == dst:
        return []
    rooms = data["rooms"]
    if src not in rooms or dst not in rooms:
        return None
    prev: dict[tuple[int, int], tuple[tuple[int, int], Exit]] = {}
    visited = {src}
    q = deque([src])
    while q:
        cur = q.popleft()
        row = rooms.get(cur)
        if not row:
            continue
        for ex in parse_room_exits(row):
            nxt = (ex.map, ex.room)
            if nxt in visited:
                continue
            if avoid_restricted and ex.is_restricted():
                continue
            if nxt not in rooms:
                continue
            visited.add(nxt)
            prev[nxt] = (cur, ex)
            if nxt == dst:
                path = []
                node = dst
                while node != src:
                    p, e = prev[node]
                    path.append(e)
                    node = p
                path.reverse()
                return path
            q.append(nxt)
    return None


if __name__ == "__main__":
    import sys, mudmap_load
    data = mudmap_load.load(
        sys.argv[1] if len(sys.argv) > 1
        else "/home/bucka/AI/mmud-explorer-src/syntax53/data-v1.11p.mdb"
    )
    start_map, start_room = 1, 1
    pos = layout(data, start_map, start_room)
    print(f"layout: {len(pos)} rooms placed from {start_map}:{start_room}")
    xs = [p[0] for p in pos.values()]
    ys = [p[1] for p in pos.values()]
    print(f"  x-range: {min(xs)}..{max(xs)}  y-range: {min(ys)}..{max(ys)}")
    # Sample pathfind
    dst_candidates = [k for k in data["rooms"] if k[0] == 1 and k[1] in (500, 1000, 1500)]
    if dst_candidates:
        for dst in dst_candidates:
            p = pathfind(data, (start_map, start_room), dst)
            print(f"path {start_map}:{start_room} -> {dst[0]}:{dst[1]} = "
                  f"{len(p) if p else '-'} steps: "
                  f"{' '.join(e.dir for e in p) if p else 'no path'}")
