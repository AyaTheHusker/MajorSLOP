"""Auto-grind script for Newhaven Arena.

Simple loop:
1. Go to Arena (down from Narrow Road)
2. Attack whatever monster is there (all-out attack)
3. After kill, loot silver/copper
4. If HP drops below threshold, flee up and rest until full
5. Repeat

Uses the command API unix socket to inject commands and reads
parser state to make decisions.
"""
import asyncio
import logging
import re
from typing import Optional, Callable, Awaitable

logger = logging.getLogger(__name__)

# HP prompt regex for parsing raw lines
RE_HP = re.compile(r'\[HP=(\d+)/MA=(\d+)\]')


class AutoGrind:
    def __init__(self, inject_fn: Callable[[str], Awaitable[None]]):
        self._inject = inject_fn
        self._running = False
        self._task: Optional[asyncio.Task] = None

        # State tracked from parser feed
        self._hp = 0
        self._max_hp = 24  # Will be learned
        self._mana = 0
        self._room = ""
        self._monsters: list[str] = []
        self._in_combat = False
        self._resting = False
        self._flee_hp = 10  # Flee when HP drops below this
        self._last_line = ""

        # Loot settings - which coin types to pick up
        self.loot_copper = True
        self.loot_silver = True
        self.loot_gold = True
        self.loot_platinum = True
        self.loot_runic = True

        # Pending loot to pick up (parsed from drop messages)
        self._pending_loot: list[str] = []

        # Encumbrance
        self._encumbrance_pct = 0
        self._encumbrance_check_kills = 0  # Check every N kills
        self._encumbrance_threshold = 30  # Convert coins at this %

        # Hostile list - monsters that have attacked us (base names, lowercase)
        # Pre-seed with known Arena hostiles
        self._hostiles: set[str] = {
            'giant rat', 'acid slime', 'lashworm', 'kobold thief',
            'carrion beast', 'filthbug',
        }

        # Events for instant reaction
        self._combat_ended = asyncio.Event()
        self._monster_spawned = asyncio.Event()
        self._got_attacked = asyncio.Event()  # Monster swung at us while idle
        self._attack_debounce = False  # Only send 1 enter per batch of swings

        # Stats
        self.kills = 0
        self.xp_earned = 0
        self.coins_looted = 0

    def feed_line(self, line: str) -> None:
        """Feed raw lines from the MUD to update grind state."""
        self._last_line = line

        # Track HP
        hp_match = RE_HP.search(line)
        if hp_match:
            self._hp = int(hp_match.group(1))
            self._mana = int(hp_match.group(2))
            if self._hp > self._max_hp:
                self._max_hp = self._hp

        # Track resting
        if '(Resting)' in line:
            self._resting = True

        # Track combat state
        if '*Combat Engaged*' in line:
            self._in_combat = True
            self._combat_ended.clear()
        if '*Combat Off*' in line:
            self._in_combat = False
            self._combat_ended.set()

        # Track kills - clear monsters list so we don't attack a corpse
        if 'falls to the ground' in line or 'dissolves into' in line or 'falls dead' in line or 'collapses' in line or 'curling tightly' in line:
            self.kills += 1
            self._monsters.clear()

        # Track XP
        xp_match = re.search(r'You gain (\d+) experience', line)
        if xp_match:
            self.xp_earned += int(xp_match.group(1))

        # Track room
        # Room names match "Area, Place" pattern
        stripped = line.strip()
        if re.match(r'^[A-Z][a-zA-Z\' ]+,\s+[A-Z][a-zA-Z\' ]+$', stripped):
            self._room = stripped
            self._resting = False  # Moved, no longer resting

        # Track monsters from "Also here:" line
        also_match = re.match(r'^Also here:\s*(.+?)\.?\s*$', stripped, re.IGNORECASE)
        if also_match:
            self._monsters = [m.strip() for m in also_match.group(1).split(',') if m.strip()]

        # Track monster spawns - if hostile, trigger instant attack
        spawn_match = re.search(r'A (.+?) (?:creeps|oozes|sneaks|wanders|slithers|crawls|scurries) in', stripped)
        if spawn_match:
            spawned = spawn_match.group(1)
            self._monsters = [spawned]
            self._monster_spawned.set()
            if self._is_hostile(spawned) and not self._in_combat and self._running:
                self._got_attacked.set()
                logger.info(f"AutoGrind: Hostile '{spawned}' spawned, attacking")

        # Track coin drops and queue loot commands
        coin_drop = re.search(r'(\d+) (copper|silver|gold|platinum|runic) (?:farthings?|nobles?|crowns?|pieces?|coins?) drop', line, re.IGNORECASE)
        if coin_drop:
            amount = coin_drop.group(1)
            coin_type = coin_drop.group(2).lower()
            self.coins_looted += int(amount)
            # Queue pickup if enabled
            abbrev = {'copper': 'c', 'silver': 's', 'gold': 'g', 'platinum': 'p', 'runic': 'r'}
            should_loot = {
                'copper': self.loot_copper,
                'silver': self.loot_silver,
                'gold': self.loot_gold,
                'platinum': self.loot_platinum,
                'runic': self.loot_runic,
            }
            if should_loot.get(coin_type, False):
                self._pending_loot.append(f"g {amount} {abbrev[coin_type]}")

        # Detect monster attacking US - "The X <verb> at you" anywhere in line
        # Lines may have ANSI escapes like [79D prepended, so use re.search
        attack_match = re.search(r'The (.+?) \w+ (?:at you|you )', stripped)
        if attack_match:
            attacker = attack_match.group(1)
            # Add to monsters list so we know what to target
            if attacker not in self._monsters:
                self._monsters.append(attacker)
            # Add base name to hostile set (strip pre-name adjectives)
            self._add_hostile(attacker)
            # If not in combat, trigger instant reaction (1 enter, then attack)
            if not self._in_combat and self._running and not self._attack_debounce:
                self._attack_debounce = True  # Only 1 enter per batch of swings
                self._got_attacked.set()
                logger.info(f"AutoGrind: Got attacked by {attacker}, triggering react")

        # Reset debounce on HP prompt (means the batch of swing messages is done)
        if RE_HP.search(line):
            self._attack_debounce = False

        # Track encumbrance from inventory output
        enc_match = re.search(r'Encumbrance:.*\[(\d+)%\]', line)
        if enc_match:
            self._encumbrance_pct = int(enc_match.group(1))

    async def start(self) -> None:
        """Start the auto-grind loop."""
        if self._running:
            return
        self._running = True
        self._task = asyncio.create_task(self._grind_loop())
        logger.info("AutoGrind: Started")

    async def stop(self) -> None:
        """Stop the auto-grind loop."""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info(f"AutoGrind: Stopped | Kills: {self.kills} | XP: {self.xp_earned}")

    async def _cmd(self, text: str) -> None:
        """Send a command and wait a tick for response."""
        await self._inject(text)
        await asyncio.sleep(0.5)

    async def _cmd_wait(self, text: str, wait: float = 2.0) -> None:
        """Send a command and wait longer (for combat rounds)."""
        await self._inject(text)
        await asyncio.sleep(wait)

    async def _wait_for_full_hp(self) -> None:
        """Rest until HP is full."""
        logger.info(f"AutoGrind: Resting... HP={self._hp}/{self._max_hp}")
        await self._cmd("rest")
        while self._running and self._hp < self._max_hp:
            await asyncio.sleep(5)
        logger.info(f"AutoGrind: Rested to full HP={self._hp}/{self._max_hp}")

    def _is_hostile(self, name: str) -> bool:
        """Check if a monster name matches the hostile list (by base name)."""
        name_lower = name.lower().strip('.')
        # Check exact match first
        if name_lower in self._hostiles:
            return True
        # Check if any hostile base name is a suffix (handles pre-name adjectives)
        # e.g. "fat giant rat" ends with "giant rat" which is in hostiles
        for hostile in self._hostiles:
            if name_lower.endswith(hostile):
                return True
        return False

    def _add_hostile(self, name: str) -> None:
        """Add a monster's base name to the hostile set."""
        name_lower = name.lower().strip('.')
        if not self._is_hostile(name_lower):
            self._hostiles.add(name_lower)
            logger.info(f"AutoGrind: Added '{name_lower}' to hostile list")

    def _get_target(self) -> Optional[str]:
        """Pick a hostile monster to attack from the room."""
        for monster in self._monsters:
            name = monster.strip('.')
            if self._is_hostile(name):
                return name
        return None

    async def _grind_loop(self) -> None:
        """Main grind loop."""
        try:
            while self._running:
                # Check if we need to rest
                if self._hp < self._flee_hp and self._room != "Newhaven, Narrow Road":
                    logger.info(f"AutoGrind: HP low ({self._hp}), fleeing!")
                    await self._cmd("up")
                    await self._wait_for_full_hp()
                    continue

                if self._hp < self._flee_hp:
                    await self._wait_for_full_hp()
                    continue

                # Navigate to Arena if not there
                if self._room != "Newhaven, Arena":
                    if self._room == "Newhaven, Narrow Road":
                        await self._cmd("down")
                    elif self._room == "Newhaven, Healer":
                        await self._cmd("e")
                        await self._cmd("down")
                    else:
                        # Try to get to Narrow Road first
                        logger.info(f"AutoGrind: Unknown room '{self._room}', pressing enter")
                        await self._cmd("")
                        continue

                # We're in the Arena
                if self._in_combat:
                    # Wait for combat to end or HP to drop
                    try:
                        await asyncio.wait_for(self._combat_ended.wait(), timeout=1)
                    except asyncio.TimeoutError:
                        # Check HP mid-combat
                        if self._hp < self._flee_hp:
                            logger.info(f"AutoGrind: HP critical ({self._hp}), fleeing!")
                            await self._inject("up")
                        continue
                    # Combat ended - fall through to loot/attack
                    self._combat_ended.clear()

                # Pick up any pending loot
                while self._pending_loot:
                    loot_cmd = self._pending_loot.pop(0)
                    await self._cmd(loot_cmd)

                # Check HP before engaging
                if self._hp < self._flee_hp:
                    logger.info(f"AutoGrind: HP low ({self._hp}), fleeing!")
                    await self._inject("up")
                    await asyncio.sleep(0.5)
                    await self._wait_for_full_hp()
                    continue

                # Check if we got attacked while idle - send 1 enter to refresh room
                if self._got_attacked.is_set():
                    self._got_attacked.clear()
                    logger.info("AutoGrind: Reacting to attack - refreshing room")
                    await self._cmd("")  # Single enter to see what's in the room
                    # Fall through to find target below

                # Find a target from known monsters
                target = self._get_target()
                if target:
                    logger.info(f"AutoGrind: Attacking {target} | HP={self._hp}")
                    self._combat_ended.clear()
                    await self._inject(f"aa {target}")
                    self._in_combat = True
                else:
                    # No monsters - wait up to 5 sec, but wake instantly on spawn/attack
                    self._monster_spawned.clear()
                    self._got_attacked.clear()
                    try:
                        spawn_t = asyncio.create_task(self._monster_spawned.wait())
                        attack_t = asyncio.create_task(self._got_attacked.wait())
                        await asyncio.wait(
                            [spawn_t, attack_t], timeout=5,
                            return_when=asyncio.FIRST_COMPLETED,
                        )
                        for t in [spawn_t, attack_t]:
                            t.cancel()
                    except asyncio.CancelledError:
                        raise
                    # Press enter to refresh room
                    await self._cmd("")

                # Periodic inventory check for encumbrance
                self._encumbrance_check_kills += 1
                if self._encumbrance_check_kills >= 5:
                    self._encumbrance_check_kills = 0
                    await self._check_encumbrance()

        except asyncio.CancelledError:
            logger.info("AutoGrind: Cancelled")
        except Exception as e:
            logger.error(f"AutoGrind: Error: {e}", exc_info=True)

    async def _check_encumbrance(self) -> None:
        """Check inventory and convert coins if too heavy."""
        await self._cmd("i")
        await asyncio.sleep(1)  # Wait for encumbrance line to parse

        if self._encumbrance_pct >= self._encumbrance_threshold:
            logger.info(f"AutoGrind: Encumbrance {self._encumbrance_pct}%, converting coins")
            await self._convert_coins()

    async def _convert_coins(self) -> None:
        """Go to shop, buy club, sell club to convert heavy coins to lighter ones."""
        was_in_arena = self._room == "Newhaven, Arena"

        # Go to weapons shop from Arena
        if self._room == "Newhaven, Arena":
            await self._cmd("up")
        # From Narrow Road, go east to Narrow Path, east to Village Entrance, north to Weapons
        if self._room == "Newhaven, Narrow Road":
            await self._cmd("e")
        if self._room == "Newhaven, Narrow Path":
            await self._cmd("e")
        if self._room == "Newhaven, Village Entrance":
            await self._cmd("n")

        # Buy and sell clubs to convert coins
        if self._room == "Newhaven, Weapons Shop":
            for _ in range(3):
                await self._cmd("buy club")
                await self._cmd("sell club")
            logger.info("AutoGrind: Coins converted")

        # Navigate back to Narrow Road for arena access
        if self._room == "Newhaven, Weapons Shop":
            await self._cmd("s")
        if self._room == "Newhaven, Village Entrance":
            await self._cmd("w")
        if self._room == "Newhaven, Narrow Path":
            await self._cmd("w")
