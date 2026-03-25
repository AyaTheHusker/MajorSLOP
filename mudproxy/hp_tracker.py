"""Monster HP tracking — estimates current HP based on damage dealt and known max HP.

Uses GameData for max HP and regen values. Tracks by index position in the room
monster list to support multiple monsters with the same name.
Provides HP fraction (0.0-1.0) for health bar rendering.
"""
import logging
import time

from .gamedata import GameData, _strip_prefix

logger = logging.getLogger(__name__)


class TrackedMonster:
    __slots__ = ("name", "base_name", "max_hp", "damage_taken", "hp_regen", "last_tick")

    def __init__(self, name: str, max_hp: int, hp_regen: int = 0):
        self.name = name
        self.base_name = _strip_prefix(name)
        self.max_hp = max_hp
        self.damage_taken = 0
        self.hp_regen = hp_regen
        self.last_tick = time.monotonic()

    @property
    def current_hp(self) -> int:
        return max(0, self.max_hp - self.damage_taken)

    @property
    def fraction(self) -> float:
        if self.max_hp <= 0:
            return 1.0
        return max(0.0, min(1.0, self.current_hp / self.max_hp))

    def apply_damage(self, amount: int) -> None:
        self.damage_taken += amount

    def apply_regen(self) -> None:
        """Apply one tick of HP regeneration."""
        if self.hp_regen > 0 and self.damage_taken > 0:
            self.damage_taken = max(0, self.damage_taken - self.hp_regen)


# Monster wound thresholds from decompiled WCCMMUD.DLL (MajorMUD v1.11p)
# Maps wound description → (min_pct, max_pct) of max HP
# We use the midpoint to estimate current HP
WOUND_LEVELS = {
    "unwounded":              (100, 100),
    "slightly wounded":       (85, 99),
    "moderately wounded":     (70, 84),
    "heavily wounded":        (50, 69),
    "severely wounded":       (30, 49),
    "critically wounded":     (20, 29),
    "very critically wounded": (0, 19),
}


class HPTracker:
    def __init__(self, gamedata: GameData):
        self._gamedata = gamedata
        # Index-based tracking: list of TrackedMonster matching room monster order
        self._tracked: list[TrackedMonster | None] = []
        self._room_monsters: list[str] = []  # current room monster names (ordered)

    def set_room_monsters(self, monsters: list[str]):
        """Update the room monster list. Preserves damage on monsters that stayed."""
        old_by_name: dict[str, list[TrackedMonster]] = {}
        for tm in self._tracked:
            if tm is not None:
                old_by_name.setdefault(tm.name.strip().lower(), []).append(tm)

        new_tracked = []
        for name in monsters:
            key = name.strip().lower()
            # Try to reuse an existing tracker for this name (FIFO order)
            if key in old_by_name and old_by_name[key]:
                tm = old_by_name[key].pop(0)
                new_tracked.append(tm)
            else:
                # Create new tracker
                max_hp = self._gamedata.get_monster_hp(name)
                if max_hp is not None:
                    regen = self._gamedata.get_monster_hp_regen(name)
                    tm = TrackedMonster(name, max_hp, regen)
                    new_tracked.append(tm)
                    logger.debug(f"Tracking monster: {name} (HP={max_hp}, regen={regen})")
                else:
                    new_tracked.append(None)

        self._tracked = new_tracked
        self._room_monsters = list(monsters)

    def record_damage(self, target_name: str, amount: int) -> tuple[int, float] | None:
        """Record damage to the first alive instance of target_name.
        Returns (index, fraction) or None if unknown."""
        key = target_name.strip().lower()
        for i, tm in enumerate(self._tracked):
            if tm is not None and tm.name.strip().lower() == key and tm.current_hp > 0:
                tm.apply_damage(amount)
                logger.debug(f"{target_name}[{i}]: took {amount} dmg, est HP {tm.current_hp}/{tm.max_hp} ({tm.fraction:.0%})")
                return (i, tm.fraction)
        return None

    def on_rest_tick(self) -> dict[int, float]:
        """Apply HP regen to all tracked monsters. Returns updated {index: fraction} dict."""
        updated = {}
        for i, tm in enumerate(self._tracked):
            if tm is not None and tm.hp_regen > 0 and tm.damage_taken > 0:
                tm.apply_regen()
                updated[i] = tm.fraction
                logger.debug(f"{tm.name}[{i}]: regen {tm.hp_regen}, est HP {tm.current_hp}/{tm.max_hp}")
        return updated

    def estimate_from_wound(self, name: str, wound_level: str) -> tuple[int, float] | None:
        """Estimate HP from a wound status description.
        Targets the first alive instance of that name.
        Returns (index, fraction) or None."""
        key = name.strip().lower()
        for i, tm in enumerate(self._tracked):
            if tm is None or tm.name.strip().lower() != key:
                continue
            if tm.max_hp <= 0:
                continue

            level = wound_level.strip().lower()
            bounds = WOUND_LEVELS.get(level)
            if bounds is None:
                return None

            min_pct, max_pct = bounds
            min_hp = int(tm.max_hp * min_pct / 100)
            max_hp_bound = int(tm.max_hp * max_pct / 100)
            est_hp = tm.current_hp

            if min_hp <= est_hp <= max_hp_bound:
                return (i, tm.fraction)

            if est_hp < min_hp:
                if tm.hp_regen > 0:
                    deficit = min_hp - est_hp
                    ticks = -(-deficit // tm.hp_regen)
                    new_hp = min(est_hp + ticks * tm.hp_regen, max_hp_bound)
                    new_hp = max(min_hp, min(new_hp, max_hp_bound))
                else:
                    new_hp = min_hp
                tm.damage_taken = tm.max_hp - new_hp
            else:
                new_hp = max_hp_bound
                tm.damage_taken = tm.max_hp - new_hp

            return (i, tm.fraction)
        return None

    def get_hp_fraction(self, index: int) -> float | None:
        """Get current HP fraction for monster at index."""
        if 0 <= index < len(self._tracked) and self._tracked[index] is not None:
            return self._tracked[index].fraction
        return None

    def get_all_fractions(self) -> dict[int, float]:
        """Get all tracked HP fractions by index."""
        result = {}
        for i, tm in enumerate(self._tracked):
            if tm is not None:
                result[i] = tm.fraction
        return result
