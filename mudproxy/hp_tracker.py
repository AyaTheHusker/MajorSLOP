"""Monster HP tracking — estimates current HP based on damage dealt and known max HP.

Uses GameData for max HP and regen values. Tracks cumulative damage per monster name.
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


class HPTracker:
    def __init__(self, gamedata: GameData):
        self._gamedata = gamedata
        self._tracked: dict[str, TrackedMonster] = {}  # lowercase name -> TrackedMonster

    def _get_or_create(self, name: str) -> TrackedMonster | None:
        """Get existing tracker or create one if monster is in gamedata."""
        key = name.strip().lower()
        if key in self._tracked:
            return self._tracked[key]

        max_hp = self._gamedata.get_monster_hp(name)
        if max_hp is None:
            return None

        regen = self._gamedata.get_monster_hp_regen(name)
        tm = TrackedMonster(name, max_hp, regen)
        self._tracked[key] = tm
        logger.debug(f"Tracking monster: {name} (HP={max_hp}, regen={regen})")
        return tm

    def record_damage(self, target: str, amount: int) -> float | None:
        """Record damage dealt to a monster. Returns HP fraction or None if unknown."""
        tm = self._get_or_create(target)
        if tm is None:
            return None
        tm.apply_damage(amount)
        logger.debug(f"{target}: took {amount} dmg, est HP {tm.current_hp}/{tm.max_hp} ({tm.fraction:.0%})")
        return tm.fraction

    def on_rest_tick(self) -> dict[str, float]:
        """Apply HP regen to all tracked monsters. Returns updated {name: fraction} dict."""
        updated = {}
        for key, tm in self._tracked.items():
            if tm.hp_regen > 0 and tm.damage_taken > 0:
                tm.apply_regen()
                updated[key] = tm.fraction
                logger.debug(f"{tm.name}: regen {tm.hp_regen}, est HP {tm.current_hp}/{tm.max_hp}")
        return updated

    def on_monster_death(self, name: str) -> None:
        """Remove monster from tracking on death."""
        key = name.strip().lower()
        if key in self._tracked:
            del self._tracked[key]
            logger.debug(f"Stopped tracking (dead): {name}")

    def on_room_change(self) -> None:
        """Clear all tracking on room change."""
        if self._tracked:
            logger.debug(f"Room change: clearing {len(self._tracked)} tracked monsters")
            self._tracked.clear()

    def get_hp_fraction(self, name: str) -> float | None:
        """Get current HP fraction for a monster, or None if not tracked."""
        key = name.strip().lower()
        tm = self._tracked.get(key)
        return tm.fraction if tm else None

    def get_all_fractions(self) -> dict[str, float]:
        """Get all tracked HP fractions."""
        return {key: tm.fraction for key, tm in self._tracked.items()}
