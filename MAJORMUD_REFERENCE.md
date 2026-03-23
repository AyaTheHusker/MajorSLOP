# MajorMUD Reference - SOS BBS (sos-bbs.net:23)

## Character
- **Name**: Bagobond Lordar
- **Race**: Elf
- **Class**: Druid
- **Level**: 1
- **HP**: 24/24
- **Mana**: 22/22
- **AC**: 11/0
- **Lives/CP**: 9/0
- **EXP**: 34 (need 2350 for level 2)
- **Strength**: 70, **Agility**: 55, **Intellect**: 50
- **Health**: 35, **Willpower**: 50, **Charm**: 50
- **Perception**: 50, **Stealth**: 34, **Spellcasting**: 50
- **Martial Arts**: 17, **MagicRes**: 50

## Equipped Gear
| Slot   | Item            |
|--------|-----------------|
| Weapon | Quarterstaff    |
| Head   | Padded helm     |
| Body   | Padded vest     |
| Legs   | Padded pants    |
| Hands  | Padded gloves   |
| Feet   | Padded boots    |

## Working Commands

### Movement
| Command | Description |
|---------|-------------|
| `n` / `s` / `e` / `w` | Move north/south/east/west |
| `se` / `nw` etc. | Diagonal movement |
| `up` / `down` | Move up/down |
| `look` | Look at current room (shows description, items, monsters, exits) |
| `look <direction>` | Peek into adjacent room without moving |
| `map` | Show area map |

### Inventory & Equipment
| Command | Description |
|---------|-------------|
| `i` | Show inventory, wealth, encumbrance |
| `stat` | Show character stats (HP, AC, class, attributes, skills) |
| `status` | Same as `stat` (full form) |
| `exp` | Show experience and level progress |
| `eq <item>` | Equip an item (e.g. `eq quarterstaff`) |
| `get <item>` / `g <item>` | Pick up item from ground |
| `drop <item>` | Drop item (must be specific enough to uniquely match) |

### Shopping
| Command | Description |
|---------|-------------|
| `list` | List items for sale in current shop |
| `buy <item>` | Buy item from shop |

### Communication
| Command | Description |
|---------|-------------|
| Just typing text | Says it out loud (e.g. `hello` = You say "hello") |
| `who` | List online players |

### Other
| Command | Description |
|---------|-------------|
| Enter (empty) | Refreshes room / shows prompt |
| `rest` | Enter resting state (greatly increases HP/Mana regen) |

## Item Naming
- MajorMUD matches items by prefix - `drop p` is too vague (matches padded, pants, etc.)
- Use enough of the name to be unique: `drop padded hel`, `buy padded vest`
- Full names always work: `buy padded boots`, `eq quarterstaff`
- Same rules apply for monster targeting: `a rat`, `a slime`, `a kobold`

## Combat & Tick System
- Game runs on **rounds** (~4-5 seconds each)
- Combat attacks happen once per round
- You can move between rounds to try to flee/shake off monsters
- Movement between rounds is key to survival at low levels
- Monsters in a room attack you every round even if you aren't in combat with them

### Combat Commands
| Command | Description |
|---------|-------------|
| `a <target>` | Attack target (e.g. `a rat`) - starts `*Combat Engaged*` |
| `aa <target>` | All-out attack - more aggressive |
| `break` | Break off combat (disengage) - shows `*Combat Off*` |
| `n`/`s`/`e`/`w`/`up` | Run a direction to flee mid-combat |
| `rest` | Rest to regenerate HP/Mana (much faster than standing) |

### Loot Commands
| Command | Description |
|---------|-------------|
| `g silv` | Pick up silver nobles |
| `g cop` | Pick up copper farthings |

### Combat Tips
- Flee (`up` from Arena) if HP gets below ~10 - acid slimes do 5+ damage per round
- Rest in a safe room (Narrow Road, Healer) to regen HP/Mana
- Resting greatly increases regen speed (~3-5 HP per tick vs ~1 standing)
- Pressing Enter or moving cancels rest state
- After killing a monster, pick up coins: `g silv` / `g cop`
- `eq <weapon>` breaks combat - need to re-engage with `a <target>` after

### Combat Messages (from log)
| Message | Meaning |
|---------|---------|
| `*Combat Engaged*` | You started fighting |
| `*Combat Off*` | Combat ended (kill, break, or equip change) |
| `You beat/smack/whap X for N damage!` | Your melee hit with weapon |
| `You punch X for N damage!` | Unarmed hit (no weapon equipped!) |
| `Your swing at X hits, but glances off its armour.` | Hit but 0 damage |
| `The X lunges at you!` | Monster attack (0 damage, dodged) |
| `The X whips you with its pseudopod for N damage!` | Slime hit |
| `Acid burns you for N damage!` | Acid DOT after slime hit |
| `The X falls to the ground...` | Monster killed |
| `You gain N experience.` | XP reward |
| `(Resting)` | Shown in prompt when resting |

### Arena Monsters Encountered
| Monster | Danger | XP | Notes |
|---------|--------|----|-------|
| giant rat | Low | 9 | Lunges, low damage, easy one-shot |
| acid slime / fierce acid slime | Medium | 16 | Pseudopod + acid DOT, can burn between rounds |
| lashworm | Low | 12 | Easy one-shot |
| kobold thief | Low-Med | 13 | Has shortsword, drops silver + copper |
| carrion beast | Medium | 17 | Snaps with teeth, one-shot with aa + quarterstaff |

**Note:** Some monsters spawn with random pre-name adjectives (small, large, fat, angry, nasty, fierce, etc.) to differentiate multiples of the same type. Not all monsters have pre-names. Target by base name: `aa rat`, `aa slime`, `aa kobold`, `aa beast`. Use pre-name to target a specific one when there are duplicates: `aa angry` vs `aa fat`.

## Newhaven Map

```
                    [Spell Shop]
                         |
[Healer] -- [Narrow Road] -- [Narrow Path] -- [Village Entrance] --SE--> [Forest Path] --S--> [Docks]
                 |    |            |   |              |    |
            [Adv Guild] |       [Gen Store]      [Weapons] [Armour]
                     [Arena]                      (north)   (south)
                     (down)
```

### Room Details
| Room | Exits | Notes |
|------|-------|-------|
| Newhaven, Healer | east | Safe room, good for resting. NPC: healer |
| Newhaven, Narrow Road | north, east, west, down | Hub area. Access to Arena below |
| Newhaven, Adventurer's Guild | south | North of Narrow Road |
| Newhaven, Arena | closed door north, up | **COMBAT ZONE** - monsters spawn here |
| Newhaven, Narrow Path | north, south, east, west | Central path |
| Newhaven, Spell Shop | south | North of Narrow Path |
| Newhaven, General Store | north | South of Narrow Path |
| Newhaven, Village Entrance | north, south, west, southeast | Town gate. Items: large sign, newbie manual |
| Newhaven, Weapons Shop | south | North of Village Entrance. NPC: Nathaniel |
| Newhaven, Armour Shop | north | South of Village Entrance. NPC: Betram |
| Newhaven, Forest Path | south, northwest | Outside town, path to docks |
| Newhaven, Docks | north | Ferryman here. Dead end |

## Shop Inventories

### Armour Shop (Betram)
| Item | Qty | Price |
|------|-----|-------|
| padded vest | 35 | Free |
| padded pants | 35 | Free |
| padded helm | 35 | Free |
| padded gloves | 35 | Free |
| padded boots | 32 | Free |

### Weapons Shop (Nathaniel)
- Quarterstaff (Free) - confirmed
- Others: TBD (need to `list` again)

## MUD Output Patterns (for parser)

### Room Block Structure
```
<Room Name>                          (e.g. "Newhaven, Healer")
    <Room description text>          (indented, may be multiple lines)
You notice <items> here.             (optional - ground items)
Also here: <entities>.               (optional - monsters/NPCs/players)
Obvious exits: <directions>          (always present)
```

### HP Prompt Format
```
[HP=XX/MA=XX]:                       (normal prompt)
[HP=XX/MA=XX]: (Resting)             (resting state)
[HP=XX/MA=XX]:<command echoed>       (command echo)
```

### Key Patterns to Parse
| Pattern | Meaning |
|---------|---------|
| `^[A-Z][a-z]+, [A-Z].*$` | Room name (always "Area, Place" format) |
| `^Obvious (?:exits\|paths): (.+)$` | Exit list |
| `^Also here: (.+)\.$` | Monsters/NPCs/players in room |
| `^You notice (.+) here\.$` | Items on ground |
| `\[HP=(\d+)/MA=(\d+)\]:` | HP/Mana prompt |
| `\*Combat Engaged\*` | Combat started |
| `\*Combat Off\*` | Combat ended |
| `You gain (\d+) experience` | XP earned |
| `for (\d+) damage!` | Damage dealt/received |
| `falls to the ground` | Monster killed |
| `You are now wearing (.+)\.` | Item equipped (armour) |
| `You are now holding (.+)\.` | Item equipped (weapon) |
| `You took (.+)\.` | Item picked up |
| `You just bought (.+) for (.+)\.` | Item purchased |
| `You are carrying (.+)` | Inventory contents |
| `Wealth: (.+)` | Money on hand |
| `There is no exit in that direction!` | Invalid movement |
| `You don't see any (.+)` | Item not found |
| `You say "(.+)"` | Accidentally said something (not a command) |
| `A (.+) sneaks into the room` | Monster spawned |
| `(Resting)` | Currently resting |

## Proxy Architecture
- Proxy listens on `127.0.0.1:9999`
- MegaMud connects to proxy, proxy connects to `sos-bbs.net:23`
- Command API on `/tmp/mudproxy_cmd.sock` (JSON over Unix socket)
- CLI: `python3 mud_cmd.py "<command>"` to inject, `python3 mud_cmd.py` for status
- Raw MUD output logged to `/tmp/mudproxy_debug.log` as `[mudproxy.raw] DEBUG:`
- GUI shows VTE terminal (left) + parsed room info (right)
- Room names match `"Area, Place"` pattern - parser updated to use strict regex
