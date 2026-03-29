# MegaMUD Reverse Engineering Reference

Complete reverse engineering documentation for megamud.exe (PE32 i386, MSVC 14.44, Feb 24 2026).
All offsets are relative to the game state struct base pointer unless noted otherwise.

## Struct Discovery

- WndProc at `0x00447050` receives struct via `GetWindowLongA(hWnd, 4)`
- STAT dump function at `0x0048a7c0` prints all fields with format strings
- INI save at `0x004322a0`, INI load at `0x004387e0` maps every stat to its offset
- Struct is heap-allocated, found by scanning writable memory for player name at `+0x537C`
- Typical base address under Wine: `0x00521398`

## Wine Quirks

- `ctypes.c_void_p` returns Python `None` for address 0, not integer 0 — always use `or 0`
- Wine maps PE `.data` sections with `PAGE_WRITECOPY` (0x08), not `PAGE_READWRITE` (0x04)
- Must include protection flags 0x08 and 0x80 in writable page filter for VirtualQueryEx
- `WriteProcessMemory` works from one Wine process to another Wine process
- CONNECTED/IN_GAME flags read as 0 through Windows API if PAGE_WRITECOPY not in filter

## MegaMUD Windows

| Window Class | Purpose | Notes |
|---|---|---|
| MMMAIN | Main game window | |
| MMTALK | Chat/input window | Has Edit child control (handle 0x10104) |
| MMPRTY | Party window | |
| #32770 | Player Statistics | Dialog — exp meter, accuracy, other stats |
| Quick Tools | Toolbar popup | |

### Player Statistics Window (dialog class #32770)

Controlled via `SendMessageA(button_hwnd, BM_CLICK, 0, 0)` — works even when window is hidden.
Session stats data lives in a **heap allocation**, NOT in the main game state struct.
Reset script: `wine python scripts/reset_stats.py [exp|accuracy|other|all]`

**Reset Buttons:**

| Control ID | Section | What it resets |
|---|---|---|
| 1721 | Experience | Duration, Exp made, Exp rate, Will level in |
| 1722 | Accuracy | Hit/Crit/BS/Extra/Cast/Round stats |
| 1723 | Other | Sneak%, Dodge%, Collected/Deposited/Stashed |

**Display Fields (Static text controls):**

| Control ID | Field | Example |
|---|---|---|
| 1134 | Duration | "00:09:36" |
| 1141 | Exp. made | "1,624" |
| 1142 | Exp. needed | "785,081" |
| 1143 | Exp. rate | "966 exp/hr" |
| 1209 | Will level in | "34:35 days" |
| 1285 | Miss % | "5%" |
| 1174 | Hit % | "77%" |
| 1175 | Hit Range | "Rng:5-21" |
| 1171 | Hit Avg | "Avg:12" |
| 1661 | Extra % | "0%" |
| 1662 | Extra Range | "Rng:0-0" |
| 1663 | Extra Avg | "Avg:0" |
| 1106 | Crit % | "18%" |
| 1107 | Crit Range | "Rng:42-82" |
| 1103 | Crit Avg | "Avg:68" |
| 1064 | BS % | "---" |
| 1065 | BS Range | "Rng:0-0" |
| 1060 | BS Avg | "Avg:0" |
| 1875 | Pre % | "N/A" |
| 1876 | Pre value | "N/A" |
| 1877 | Pre Range | "Rng:0-0" |
| 1878 | Pre Avg | "Avg:0" |
| 1021 | Cast label | "Cast:" |
| 1080 | Cast % | "N/A" |
| 1081 | Cast Range | "Rng:0-0" |
| 1077 | Cast Avg | "Avg:0" |
| 1419 | Round Range | "Rng:6-95" |
| 1418 | Round Avg | "Avg:40" |
| 1442 | Sneak % | "0%" |
| 1127 | Dodge % | "0%" |
| 1883 | Collected copper | "0" |
| 1884 | Collected items | "0" |
| 1118 | Deposit/Sold copper | "0" |
| 1885 | Deposit/Sold items | "0" |
| 1882 | Stashed copper | "0" |
| 1886 | Stashed items | "0" |
| 1635 | Income rate | "0 copper/hr" |
| 1550 | Copy button | Copies accuracy text to clipboard |

---

## Struct Offset Map

### Window / UI

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x000C | ptr | HWND_OFFSET | HWND of main window |
| 0x014C | i32 | SOUND_ENABLED | Sound on/off |
| 0x0150 | i32 | SHOW_TALK | Show talk window |
| 0x0154 | i32 | SHOW_WHO | Show who list |
| 0x0158 | i32 | SHOW_STAT | Show stats |
| 0x015C | i32 | SHOW_SESS | Show session |
| 0x0160 | i32 | SHOW_TIME | Show time |
| 0x0164 | i32 | SHOW_RATE | Show rate |
| 0x0168 | i32 | SHOW_PARTY | Show party |

### Last Actions (Debug/Trace)

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x01A8 | i32[10] | LAST_ACTION_IDS | Action type IDs (10 slots) |
| 0x01D0 | char[50][10] | LAST_ACTION_STRS | Action description strings (0x32 bytes each) |

### Entity List

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x1ED4 | i32 | ENTITY_LIST_COUNT | Number of entities in current room |
| 0x1EE0 | ptr | ENTITY_LIST_PTR | Pointer to entity array |

Entity array element:
- `+0x00` i32: type (1=player, 2=monster, 3=item)
- `+0x0C` i32: quantity
- `+0x10` char[]: name
- `+0x34` i32: known flag 1
- `+0x38` i32: known flag 2

### Room / Navigation

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x2C18 | i32 | ROOM_DB_COUNT | Number of rooms in database |
| 0x2C20 | ptr | ROOM_DB_PTR | Pointer to room entry array (checksum at +0x44, name at +0x06) |
| 0x2D9C | char[] | ROOM_NAME | Current room name (null-terminated ASCII) |
| 0x2E70 | u32 | ROOM_CHECKSUM | Current room checksum/hash |
| 0x2E78 | i32[10] | ROOM_EXITS | Exit types per direction (0=none, 2=closed, 3=open, 4=special, 5=unlocked) |
| 0x2EA4 | u32 | DEST_CHECKSUM | **WRITABLE** — destination room checksum for goto |
| 0x3154 | ptr | ROOM_MATCH_PTR | Room matching / memory alloc related |
| 0x314C | ptr | ROOM_MATCH_2 | Room matching related |
| 0x358C | i32 | BACKTRACK_ROOMS | Rooms to backtrack |
| 0x5508 | i32 | RUN_AWAY_ROOMS | Rooms to run away |
| 0x5528 | i32 | ROOM_DARKNESS | 0=normal, 2=dark, 3=pitch black |

### Party — Runtime State

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x35F0 | i32 | PARTY_SIZE | Number of party members |
| 0x35F8 | struct[N] | PARTY_MEMBERS | Party member array, each 0x38 (56) bytes |
| 0x3748 | char[] | PARTY_LEADER_NAME | Party leader name |
| 0x5788 | i32 | FOLLOWING | Following flag |
| 0x578C | i32 | LOST_FROM_LEADER | Lost from leader |
| 0x5790 | i32 | WAITING_ON_MEMBER | Waiting on member |
| 0x5794 | i32 | HOLDING_UP_LEADER | Holding up leader |

Party member struct (0x38 bytes each):
- `+0x00` char[]: name
- `+0x0C` i32: HP percentage
- `+0x10` i32: current HP
- `+0x14` i32: max HP
- `+0x28` i32: flags (bit 7=hungup, bit 3=missing, bit 2=invited)

### Party Settings (0x3548–0x35E0)

All WRITABLE. From Settings → Party tab.

| Offset | Size | Name | INI Key | UI Label | Description |
|---|---|---|---|---|---|
| 0x3548 | i32 | PARTY_RANK | PartyRank | Party Rank (Front/Mid/Back) | 1=front, 2=mid, 3=back |
| 0x354C | i32 | ATTACK_LAST | AttackLast | ☑ Attack last in party | Attack after other party members |
| 0x3550 | i32 | SHARE_DAMAGE | ShareDamage | ☑ Share damage | Split damage with party |
| 0x3554 | i32 | ASK_HEALTH | AskHealth | ☑ Request party health | Auto-request party HP status |
| 0x355C | i32 | DEFEND_PARTY | DefendParty | ☑ Defend party when PvP | Defend party in PvP |
| 0x3560 | i32 | ATT_LEADER_MSTR | AttLeaderMstr | ☑ Attack what other members attack | Follow leader's target |
| 0x3564 | i32 | SHARE_CASH | ShareCash | ☑ Auto-share collected cash | Split cash with party |
| 0x3574 | i32 | HELP_BASH | HelpBash | ☑ Help leader bash doors | Assist bashing |
| 0x3578 | i32 | IGNORE_PARTY | IgnoreParty | ☑ Ignore @party when following | Ignore party commands |
| 0x357C | i32 | IGNORE_WAIT | IgnoreWait | ☑ Ignore @wait when leading | Ignore wait requests |
| 0x3580 | i32 | IGNORE_PANIC | IgnorePanic | ☑ Ignore @panic when leading | Ignore panic signals |
| 0x3584 | i32 | SEND_PANIC | SendPanic | ☑ Say @panic when leading | Send panic signal |
| 0x3590 | i32 | PAR_PERIOD | ParPeriod | 'pa' frequency (seconds) | Party status poll interval |
| 0x3594 | i32 | PARTY_MAX_MSTRS | PartyMaxMstrs | Max. monsters when partying | Max monsters to engage in party |
| 0x3598 | i32 | PARTY_MAX_EXP | PartyMaxExp | Max. monster experience | Max monster exp in party |
| 0x35A0 | i32 | PARTY_HEAL1_PCT | PartyHeal1% | Party Heal — Minor at _% | Minor heal HP threshold |
| 0x35A4 | i32 | PARTY_HEAL2_PCT | PartyHeal2% | Party Heal — Major at _% | Major heal HP threshold |
| 0x35A8 | i32 | PARTY_ASK_HEAL_PCT | PartyAskHeal% | Request healing at _% | Request heal from party |
| 0x35AC | i32 | PARTY_WAIT_PCT | PartyWait% | Wait if members are below _% | Wait for low-HP members |
| 0x35B0 | i32 | PARTY_WAIT_MAX | PartyWaitMax | If leading, wait only (mins) | Max wait time in minutes |
| 0x35B4 | i32 | PARTY_BLESS_WAIT1 | PartyBlessWait1 | Bless 1 Timeout (secs) | Re-cast timer for bless 1 |
| 0x35B8 | i32 | PARTY_BLESS_WAIT2 | PartyBlessWait2 | Bless 2 Timeout (secs) | Re-cast timer for bless 2 |
| 0x35BC | i32 | PARTY_BLESS_WAIT3 | PartyBlessWait3 | Bless 3 Timeout (secs) | Re-cast timer for bless 3 |
| 0x35C0 | i32 | PARTY_BLESS_WAIT4 | PartyBlessWait4 | Bless 4 Timeout (secs) | Re-cast timer for bless 4 |
| 0x35C4 | char[21] | PARTY_HEAL1_CMD | PartyHeal1 | Party Heal 1 spell | Heal spell command (minor) |
| 0x35D9 | char[21] | PARTY_HEAL2_CMD | PartyHeal2 | Party Heal 2 spell | Heal spell command (major) |
| 0x1EE4 | i32 | ATTACK_REVERSE | AttackReverse | ☑ Attack in reverse order | Reverse attack order |

Party bless spell strings (in a separate region):

| Offset | Size | Name | INI Key | UI Label |
|---|---|---|---|---|
| 0x4FF1 | char[21] | PARTY_BLESS1_CMD | PartyBless1 | Party Bless 1 spell |
| 0x5006 | char[21] | PARTY_BLESS2_CMD | PartyBless2 | Party Bless 2 spell |
| 0x501B | char[21] | PARTY_BLESS3_CMD | PartyBless3 | Party Bless 3 spell |
| 0x5030 | char[21] | PARTY_BLESS4_CMD | PartyBless4 | Party Bless 4 spell |

### Health Thresholds (0x3788–0x37C8)

All WRITABLE. Sequential i32 DWORDs. Controls when MegaMUD auto-rests, heals, runs, or logs off.

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x3788 | i32 | HP_FULL_PCT | HpFull% | HP % considered "full" (stop resting) |
| 0x378C | i32 | HP_REST_PCT | HpRest% | Rest when HP below this % |
| 0x3790 | i32 | HP_HEAL_PCT | HpHeal% | Cast heal when HP below this % |
| 0x3794 | i32 | HP_HEAL_ATT_PCT | HpHealAtt% | Heal during combat when below this % |
| 0x3798 | i32 | HP_RUN_PCT | HpRun% | Run away when HP below this % (0=disabled) |
| 0x379C | i32 | HP_LOGOFF_PCT | HpLogoff% | Emergency logoff HP threshold |
| 0x37A0 | i32 | HP_HEAL_PERIOD | HpHealPeriod | Heal check interval in seconds |
| 0x37A4 | i32 | MANA_FULL_PCT | ManaFull% | Mana % considered "full" |
| 0x37A8 | i32 | MANA_REST_PCT | ManaRest% | Rest when mana below this % |
| 0x37AC | i32 | MANA_HEAL_PCT | ManaHeal% | Min mana % to cast heal |
| 0x37B0 | i32 | MANA_HEAL_ATT_PCT | ManaHealAtt% | Min mana % to heal in combat |
| 0x37B4 | i32 | MANA_RUN_PCT | ManaRun% | Run when mana below this % |
| 0x37B8 | i32 | MANA_BLESS_PCT | ManaBless% | Min mana % to cast bless |
| 0x37BC | i32 | MANA_MULT_ATT_PCT | ManaMultAtt% | Min mana % for multi-attack spell |
| 0x37C0 | i32 | MANA_PRE_ATT_PCT | ManaPreAtt% | Min mana % for pre-attack spell |
| 0x37C4 | i32 | MANA_ATTACK_PCT | ManaAttack% | Min mana % for attack spell |
| 0x37C8 | i32 | USE_MEDITATE | UseMeditate | Use meditate instead of rest |

### Cash / Item Pickup (0x3208–0x3260)

Gated by AUTO_CASH (0x4D14) and AUTO_GET (0x4D18).

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x3208 | i32 | WANT_COPPER | WantCopper | Pick up copper coins |
| 0x320C | i32 | WANT_SILVER | WantSilver | Pick up silver coins |
| 0x3210 | i32 | WANT_GOLD | WantGold | Pick up gold coins |
| 0x3214 | i32 | WANT_PLAT | WantPlat | Pick up platinum coins |
| 0x3218 | i32 | WANT_RUNIC | WantRunic | Pick up runic coins |
| 0x321C | i32 | STASH_COIN | StashCoin | Auto-stash coins |
| 0x3220 | i32 | DONT_BE_HEAVY | DontBeHeavy | Avoid heavy encumbrance |
| 0x3224 | i32 | DONT_BE_MEDIUM | DontBeMedium | Avoid medium encumbrance |
| 0x3228 | i32 | DROP_COINS | DropCoins | Drop unwanted coin types |
| 0x322C | i32 | LIMIT_WEALTH | LimitWealth | Enable wealth limit |
| 0x3230 | i32 | MIN_WEALTH | MinWealth | Minimum wealth threshold |
| 0x3234 | i32 | MAX_WEALTH | MaxWealth | Maximum wealth threshold |
| 0x3238 | i32 | LIMIT_COINS | LimitCoins | Enable coin count limit |
| 0x323C | i32 | MAX_COINS | MaxCoins | Max coins to carry |
| 0x3240 | char[] | BANK_NAME | Bank | Bank room name for deposits |

### Timers

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x35B0 | i32 | TIMER_WAIT | Used in party wait calculation |
| 0x3A64 | i32 | GANGPATH_FLAG | Gang path related |

### Player Identity

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x537C | char[] | PLAYER_NAME | Player name (null-terminated ASCII) |
| 0x5387 | char[] | PLAYER_SURNAME | Player surname |
| 0x539A | char[] | PLAYER_GANG | Gang name |

### Player Experience (64-bit)

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x53B0 | u32 | PLAYER_EXP_LO | Experience low dword |
| 0x53B4 | u32 | PLAYER_EXP_HI | Experience high dword |
| 0x53B8 | u32 | PLAYER_NEED_LO | Exp needed to level (low dword) |
| 0x53BC | u32 | PLAYER_NEED_HI | Exp needed to level (high dword) |

### Player Core

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x53C0 | i32 | PLAYER_LIVES | Lives remaining |
| 0x53C4 | i32 | PLAYER_RACE | Race ID |
| 0x53C8 | i32 | PLAYER_CLASS | Class ID |
| 0x53D0 | i32 | PLAYER_LEVEL | Player level |
| 0x53D4 | i32 | PLAYER_CUR_HP | Current HP |
| 0x53DC | i32 | PLAYER_MAX_HP | Maximum HP |
| 0x53E0 | i32 | PLAYER_CUR_MANA | Current mana |
| 0x53E8 | i32 | PLAYER_MAX_MANA | Maximum mana |
| 0x53EC | i32 | PLAYER_ENCUMB_LO | Encumbrance current |
| 0x53F0 | i32 | PLAYER_ENCUMB_HI | Encumbrance max |

### Player Stats

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x53F4 | i32 | PLAYER_STRENGTH | |
| 0x53F8 | i32 | PLAYER_AGILITY | |
| 0x53FC | i32 | PLAYER_INTELLECT | |
| 0x5400 | i32 | PLAYER_WISDOM | |
| 0x5404 | i32 | PLAYER_HEALTH | Constitution/health stat |
| 0x5408 | i32 | PLAYER_CHARM | |
| 0x540C | i32 | PLAYER_PERCEPTION | |
| 0x5410 | i32 | PLAYER_STEALTH | |
| 0x5414 | i32 | PLAYER_THIEVERY | |
| 0x5418 | i32 | PLAYER_TRAPS | |
| 0x541C | i32 | PLAYER_PICKLOCKS | |
| 0x5420 | i32 | PLAYER_TRACKING | |
| 0x5424 | i32 | PLAYER_MARTIAL_ARTS | |
| 0x5428 | i32 | PLAYER_MAGIC_RES | |
| 0x542C | i32 | PLAYER_SPELL_CASTING | |

### Player Flags / On-Entry

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x54B4 | i32 | ON_ENTRY_ACTION | 0=nothing, 1=resume loop, 2=auto-roam, 3=? |
| 0x54BC | i32 | MODE | 11=idle, 14=walking/running, 17=lost (re-syncing) |
| 0x54D4 | i32 | MSG_CODE | Last WM_ message code received |
| 0x54D8 | i32 | STEPS_REMAINING | Countdown of steps remaining in current path segment |
| 0x54DC | i32 | UNK_54DC | Set to -1 before loop, cleared to 0 on loop start |

### Connection

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x563C | i32 | CONNECTED | Non-zero if connected to BBS |
| 0x5644 | i32 | IN_GAME | Non-zero if in the game world |
| 0x564C | i32 | GO_FLAG | **WRITABLE** — master go/stop toggle. 1=go, 0=stop. Stop button clears this. Required for walk/run/loop to work. |
| 0x565C | i32 | FIX_STEPS | |

### Pathing / Loop Control

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x5664 | i32 | PATHING_ACTIVE | **WRITABLE** — 1 = currently pathing/walking |
| 0x5668 | i32 | LOOPING | **WRITABLE** — 1 = loop mode (wrap path back to start) |
| 0x566C | i32 | AUTO_ROAMING | Auto-roaming flag |
| 0x5670 | i32 | IS_LOST | Player is lost |
| 0x5674 | i32 | IS_FLEEING | Player is fleeing |
| 0x5678 | i32 | IS_RESTING | Player is resting |
| 0x567C | i32 | IS_MEDITATING | Player is meditating |
| 0x5688 | i32 | IS_SNEAKING | Player is sneaking |
| 0x5690 | i32 | IS_HIDING | Player is hiding |
| 0x5698 | i32 | IN_COMBAT | Player is in combat |
| 0x569C | i32 | UNK_569C | Tracks with IN_COMBAT — likely IS_ATTACKING or ATTACK_PENDING |

### Status Effects

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x56AC | i32 | IS_BLINDED | |
| 0x56B0 | i32 | IS_CONFUSED | |
| 0x56B4 | i32 | IS_POISONED | |
| 0x56B8 | i32 | POISON_AMOUNT | Poison damage per tick |
| 0x56BC | i32 | IS_DISEASED | |
| 0x56C0 | i32 | IS_LOSING_HP | HP drain active |
| 0x56C4 | i32 | IS_REGENERATING | |
| 0x56C8 | i32 | IS_FLUXING | |
| 0x56CC | i32 | IS_HELD | |
| 0x56D0 | i32 | IS_STUNNED | |
| 0x56E0 | i32 | RESTING_TO_FULL_HP | |
| 0x56E4 | i32 | RESTING_TO_FULL_MANA | |

### Automation Toggles (0x4D00–0x4D28)

All 11 toolbar auto-switches, sequential i32 DWORDs. **ALL WRITABLE.**
Matches INI keys: AutoCombat, AutoNuke, AutoHeal, AutoBless, AutoLight, AutoCash, AutoGet, AutoSearch, AutoSneak, AutoHide, AutoTrack.

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x4D00 | i32 | AUTO_COMBAT | AutoCombat | Auto-combat on/off. Walk sets to 1, Run sets to 0 (re-enables on arrival). |
| 0x4D04 | i32 | AUTO_NUKE | AutoNuke | Auto-nuke (offensive spells) |
| 0x4D08 | i32 | AUTO_HEAL | AutoHeal | Auto-heal self |
| 0x4D0C | i32 | AUTO_BLESS | AutoBless | Auto-bless (buff spells) |
| 0x4D10 | i32 | AUTO_LIGHT | AutoLight | Auto-light in dark rooms |
| 0x4D14 | i32 | AUTO_CASH | AutoCash | Auto-pick up cash |
| 0x4D18 | i32 | AUTO_GET | AutoGet | Auto-pick up items |
| 0x4D1C | i32 | AUTO_SEARCH | AutoSearch | Auto-search rooms |
| 0x4D20 | i32 | AUTO_SNEAK | AutoSneak | Auto-sneak movement |
| 0x4D24 | i32 | AUTO_HIDE | AutoHide | Auto-hide after combat |
| 0x4D28 | i32 | AUTO_TRACK | AutoTrack | Auto-track monsters |

### Default Profiles (saved toggle presets)

Profile 1 (0x4D2C–0x4D54) and Profile 2 (0x4D58–0x4D80) — same 11-switch layout as above.
INI keys: DefCombat1/DefNuke1/...DefTrack1, DefCombat2/DefNuke2/...DefTrack2.
Used by "All Off" / mode switching to restore previous toggle states.

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x4D2C | i32[11] | DEF_PROFILE_1 | Default profile 1 (same order as auto-toggles) |
| 0x4D58 | i32[11] | DEF_PROFILE_2 | Default profile 2 (same order as auto-toggles) |
| 0x4D84 | i32 | START_TASK | INI StartTask — on-login task (0=nothing) |

### Combat Settings (from binary disassembly)

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x37D0 | i32 | CAN_BACKSTAB | CanBackStab | Character can backstab |
| 0x37D4 | i32 | DONT_BS_IF_MULTI | DontBsIfMulti | Don't backstab if multiple monsters |
| 0x37D8 | i32 | RUN_IF_BS_FAILS | RunIfBsFails | Run if backstab fails |
| 0x37DC | i32 | ATTACK_NEUTRAL | AttackNeutral | Attack neutral monsters |
| 0x37E0 | i32 | POLITE_ATTACKS | PoliteAttacks | Polite attack mode |
| 0x4D88 | i32 | MAX_MSTRS | MaxMstrs | Max monsters to engage |
| 0x4D90 | i32 | MAX_MSTR_EXP | MaxMstrExp | Max monster exp to engage |
| 0x4DAC | i32 | MAX_CAST_CNT | MaxCastCnt | Max spell casts per round |
| 0x4DB0 | i32 | ATT_MAX_DMG | AttMaxDmg | Attack spell max damage |
| 0x4DB4 | i32 | PRE_CAST_CNT | PreCastCnt | Pre-attack cast count |
| 0x4DB8 | i32 | PRE_MAX_DMG | PreMaxDmg | Pre-attack max damage |
| 0x4DBC | i32 | MULT_CAST_CNT | MultCastCnt | Multi-attack cast count |
| 0x4DC0 | i32 | MULT_MSTR_CNT | MultMstrCnt | Min enemies for multi-attack |
| 0x4DC4 | i32 | MULT_MAX_DMG | MultMaxDmg | Multi-attack max damage |
| 0x54E4 | i32 | BASH_MAX | BashMax | Max bash attempts per door |
| 0x54EC | i32 | PICK_MAX | PickMax | Max pick-lock attempts |
| 0x54F4 | i32 | DISARM_MAX | DisarmMax | Max disarm trap attempts |
| 0x54FC | i32 | SEARCH_MAX | SearchMax | Max searches per room |
| 0x550C | i32 | RUN_ROOMS | RunRooms | Rooms to flee when running |
| 0x574C | i32 | AUTOCOMBAT_SLOT | — | -1 when certain auto functions triggered |

### Behavior / Other Settings (from binary disassembly)

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x1E18 | i32 | DISABLE_EVENTS | DisableEvents | Disable event system |
| 0x1E1C | i32 | EVENTS_AFK_ONLY | EventsAfkOnly | Only run events when AFK |
| 0x37C8 | i32 | CAN_PICK_LOCKS | CanPickLocks | Character can pick locks |
| 0x37CC | i32 | CAN_DISARM_TRAPS | CanDisarmTraps | Character can disarm traps |
| 0x3780 | i32 | AUTO_TRAIN | AutoTrain | Auto-train on level up |
| 0x3810 | i32 | RELOG_INSTEAD | RelogInstead | Relog instead of hangup |
| 0x3814 | i32 | HANGUP_NOT_AFK | HangupNotAfk | Hangup if not AFK |
| 0x3818 | i32 | HANGUP_ALL_OFF | HangupAllOff | Turn all off on hangup |
| 0x381C | i32 | HANGUP_NAKED | HangupNaked | Unequip on hangup |
| 0x3A34 | i32 | IGNORE_POISON | IgnorePoison | Don't auto-cure poison |
| 0x3A38 | i32 | IGNORE_BLIND | IgnoreBlind | Don't auto-cure blind |
| 0x3A3C | i32 | IGNORE_CONFUSION | IgnoreConfusion | Don't auto-cure confusion |
| 0x3A40 | i32 | BLESS_RESTING | BlessResting | Cast bless while resting |
| 0x3A44 | i32 | BLESS_COMBAT | BlessCombat | Cast bless during combat |
| 0x3A48 | i32 | SEARCH_NEED_ITEM | SearchNeedItem | Only search if item expected |
| 0x3A4C | i32 | RUN_BACKWARDS | RunBackwards | Run backwards when fleeing |
| 0x3A50 | i32 | BREAK_B4_RUNNING | BreakB4Running | Break stealth before running |
| 0x5810 | i32 | LIGHT_DIM_ROOMS | LightDimRooms | Auto-light dim rooms |
| 0x5814 | i32 | MUST_SNEAK | MustSneak | Must sneak movement |
| 0x5818 | i32 | SUPER_STEALTH | SuperStealth | Supernatural stealth mode |
| 0x581C | i32 | AUTO_AUTO_COMBAT | AutoAutoCombat | Re-enable combat on arrival |
| 0x5820 | i32 | AUTO_AUTO_HEAL | AutoAutoHeal | Re-enable heal on arrival |
| 0x8AA0 | i32 | AUTO_CONNECT | AutoConnect | Auto-reconnect on disconnect |
| 0x4D84 | i32 | NO_MODE_DEFS | NoModeDefs | Disable mode defaults |

### Spell Commands (Settings → Spells tab)

All char[21] string buffers unless noted.

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x4E40 | char[21] | HEAL_CMD | HealCmd | Heal spell command |
| 0x4E55 | char[21] | REGEN_CMD | RegenCmd | Regen spell command |
| 0x4E6A | char[21] | FLUX_CMD | FluxCmd | Flux spell command |
| 0x4E80 | i32 | FLUX_MIN | FluxMin | Minimum mana for flux |
| 0x4E88 | i32 | MEDITATE_B4_REST | MeditateB4Rest | Meditate before resting |
| 0x4E8C | char[21] | BLIND_CMD | BlindCmd | Cure blindness command |
| 0x4EA1 | char[21] | POISON_CMD | PoisonCmd | Cure poison command |
| 0x4EB6 | char[21] | DISEASE_CMD | DiseaseCmd | Cure disease command |
| 0x4ECB | char[21] | FREEDOM_CMD | FreedomCmd | Cure held/freedom command |
| 0x4EE0 | char[21] | LIGHT_CMD | LightCmd | Light spell command |
| 0x4EF5 | char[21] | HP_FULL_CMD | HpFullCmd | Full HP heal command |
| 0x4F0A | char[21] | MA_FULL_CMD | MaFullCmd | Full mana restore command |
| 0x4F1F | char[21] | BLESS_CMD1 | BlessCmd1 | Bless spell 1 |
| 0x4F34 | char[21] | BLESS_CMD2 | BlessCmd2 | Bless spell 2 |
| 0x4F49 | char[21] | BLESS_CMD3 | BlessCmd3 | Bless spell 3 |
| 0x4F5E | char[21] | BLESS_CMD4 | BlessCmd4 | Bless spell 4 |
| 0x4F73 | char[21] | BLESS_CMD5 | BlessCmd5 | Bless spell 5 |
| 0x4F88 | char[21] | BLESS_CMD6 | BlessCmd6 | Bless spell 6 |
| 0x4F9D | char[21] | BLESS_CMD7 | BlessCmd7 | Bless spell 7 |
| 0x4FB2 | char[21] | BLESS_CMD8 | BlessCmd8 | Bless spell 8 |
| 0x4FC7 | char[21] | BLESS_CMD9 | BlessCmd9 | Bless spell 9 |
| 0x4FDC | char[21] | BLESS_CMD10 | BlessCmd10 | Bless spell 10 |
| 0x5045 | char[] | PRE_REST_CMD | PreRestCmd | Command before resting |
| 0x50AA | char[] | POST_REST_CMD | PostRestCmd | Command after resting |

### Combat String Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x51D9 | char[] | ATTACK_CMD | AttackCmd | Attack command (e.g. "a") |
| 0x51E4 | char[] | MULT_ATTACK_CMD | MultAttack | Multi-attack spell command |
| 0x51EF | char[] | PRE_ATTACK_CMD | PreAttack | Pre-attack spell command |
| 0x51FA | char[] | ATTACK_SPL_CMD | AttackSpl | Attack spell command |
| 0x52C6 | char[] | NRM_WEAPON | NrmWeapon | Normal weapon name |
| 0x52E5 | char[] | BS_WEAPON | BsWeapon | Backstab weapon name |
| 0x5304 | char[] | ALT_WEAPON | AltWeapon | Alternate weapon name |
| 0x5323 | char[] | SHIELD_ITEM | Shield | Combat shield item |
| 0x5354 | i32 | USE_SHIELD_FOR_BS | UseShieldForBS | Equip shield for backstab |
| 0x5358 | i32 | USE_NRM_WEAP_FOR_SPELLS | UseNrmWeapForSpells | Use normal weapon for spells |

### Talk / AFK Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x3820 | i32 | AUTO_AFK | AutoAfk | Auto-AFK when idle |
| 0x3824 | i32 | AUTO_AFK_OFF | AutoAfkOff | Auto-disable AFK on activity |
| 0x3828 | i32 | AFK_MINIMIZED | AfkMinimized | Go AFK when minimized |
| 0x382C | i32 | AFK_TIMEOUT | AfkTimeout | AFK timeout in minutes |
| 0x3830 | char[] | AFK_REPLY | AfkReply | AFK auto-reply message |
| 0x3931 | char[] | CMD_REPLY | CmdReply | Invalid command reply |
| 0x3A54 | i32 | LOG_TALK | LogTalk | Enable talk logging |
| 0x3A58 | i32 | GREET_PLAYERS | GreetPlayers | Auto-greet players |
| 0x3A5C | i32 | LOOK_PLAYERS | LookPlayers | Auto-look at players |
| 0x3A60 | i32 | NO_REMOTE_CMDS | NoRemoteCmds | Disable remote commands |
| 0x3A68 | i32 | WARN_REMOTE | WarnRemote | Warn on remote commands |
| 0x3A6C | i32 | DIVERT_LOCAL | DivertLocal | Divert local chat to talk window |
| 0x3A70 | i32 | DIVERT_TELE | DivertTele | Divert telepathy |
| 0x3A74 | i32 | DIVERT_GOSSIP | DivertGossip | Divert gossip |
| 0x3A78 | i32 | DIVERT_GANG | DivertGang | Divert gang chat |
| 0x3A7C | i32 | DIVERT_BROAD | DivertBroad | Divert broadcast |
| 0x1707 | char[] | LOG_FILE | LogFile | Talk log file path |

### Display Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x017C | i32 | POPUP_AFK_MSGS | PopupAfkMsgs | Popup AFK messages |
| 0x0180 | i32 | SHOW_ALL_AFK | ShowAllAfk | Show all AFK messages |
| 0x0184 | i32 | AUTO_HIDE_PARTY | AutoHideParty | Auto-hide party window |
| 0x1A44 | i32 | SCALE_ANSI | ScaleAnsi | Scale ANSI display |
| 0x1A48 | i32 | ANSI_ROWS | AnsiRows | Terminal rows |
| 0x1A4C | i32 | ANSI_COLS | AnsiCols | Terminal columns |
| 0x3800 | i32 | SHOW_INFO | ShowInfo | Show info messages |
| 0x3804 | i32 | SHOW_ROUNDS | ShowRounds | Show combat rounds |
| 0x3808 | i32 | SHOW_SEND | ShowSend | Show sent commands |
| 0x380C | i32 | SHOW_RUN_MSG | ShowRunMsg | Show run messages |

### PvP Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x3A80 | i32 | PVP_ACTION | PvpAction | PvP response action |
| 0x3A84 | i32 | REDIAL_PVP | RedialPvp | Redial after PvP death |
| 0x3A8C | i32 | FLEE_ROOMS_PVP | FleeRooms | Rooms to flee in PvP |
| 0x3A90 | i32 | FLEE_TIMEOUT | FleeTimeout | Flee timeout (seconds) |
| 0x3A94 | i32 | TRACK_ENEMIES | TrackEnemies | Auto-track enemy players |
| 0x3A9C | i32 | TRACK_DELAY | TrackDelay | Delay between tracking (seconds) |
| 0x3AA0 | i32 | HIDE_DELAY | HideDelay | Delay before hiding (seconds) |
| 0x3AA4 | i32 | NOTIFY_GANG | NotifyGang | Notify gang on PvP |
| 0x3AA8 | char[] | PVP_FLEE_ROOM | PvpFleeRoom | Room to flee to |
| 0x5205 | char[] | PVP_SPELL1 | PvpSpell1 | PvP attack spell 1 |
| 0x5256 | char[] | PVP_SPELL2 | PvpSpell2 | PvP attack spell 2 |
| 0x8A88 | i32 | PVP_SAFE_PERIOD | PvpSafePeriod | Safe period after PvP (seconds) |

### Alerts (each char[257] — sound file path)

| Offset | Size | Name | INI Key |
|---|---|---|---|
| 0x3AD8 | char[257] | ALERT_IDLE | AlertIdle |
| 0x3BD9 | char[257] | ALERT_TRAIN | AlertTrain |
| 0x3CDA | char[257] | ALERT_LOW_HPS | AlertLowHPs |
| 0x3DDB | char[257] | ALERT_HANGUP | AlertHangup |
| 0x3EDC | char[257] | ALERT_PVP | AlertPvP |
| 0x3FDD | char[257] | ALERT_TELE | AlertTele |
| 0x40DE | char[257] | ALERT_PAGE | AlertPage |
| 0x41DF | char[257] | ALERT_TALK | AlertTalk |
| 0x42E0 | char[257] | ALERT_GANGPATH | AlertGangpath |
| 0x43E1 | char[257] | ALERT_GOSSIP | AlertGossip |
| 0x44E2 | char[257] | ALERT_AUCTION | AlertAuction |
| 0x45E3 | char[257] | ALERT_BROADCAST | AlertBroadcast |
| 0x46E4 | i32 | ALERT_WHEN_AFK | AlertWhenAfk |

### Auto-roam Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x3274 | u16 | AUTO_MOVE | AutoMove | Direction bitmask (255=all) |
| 0x3276 | u16 | AUTO_DOOR | AutoDoor | Auto-open doors |
| 0x3278 | char[41]×10 | AUTO_ROOM_BASE | AutoRoom0-9 | Room name filters (stride 0x29) |
| 0x3412 | char[31]×10 | AUTO_CMD_BASE | AutoCmd0-9 | Commands per room (stride 0x1F) |

### Comms / Connection Settings

| Offset | Size | Name | INI Key | Description |
|---|---|---|---|---|
| 0x8A54 | i32 | REDIAL_PAUSE | RedialPause | Pause between redials (seconds) |
| 0x8A5C | i32 | REDIAL_MAX | RedialMax | Max redial attempts |
| 0x8A60 | i32 | REDIAL_CONNECT | RedialConnect | Redial on disconnect |
| 0x8A64 | i32 | REDIAL_CARRIER | RedialCarrier | Redial on carrier loss |
| 0x8A68 | i32 | REDIAL_NO_RESPONSE | RedialNoResponse | Redial on no response |
| 0x8A6C | i32 | REDIAL_CLEANUP | RedialCleanup | Cleanup on redial |
| 0x8A70 | i32 | LOGOFF_LOW_EXP | LogoffLowExp | Logoff if exp rate low |
| 0x8A84 | i32 | CLEANUP_PERIOD | CleanupPeriod | Cleanup period (minutes) |
| 0x8A94 | i32 | MIN_EXP_RATE | MinExpRate | Minimum exp rate threshold |
| 0x8A98 | i32 | LAG_WAIT | LagWait | Lag wait (seconds) |
| 0x8AA0 | i32 | AUTO_CONNECT | AutoConnect | Auto-reconnect on disconnect |
| 0x9444 | i32 | AUTO_ZMODEM | AutoZModem | Auto ZModem file transfer |

### Path Data (loaded from .mp file)

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x5820 | i32 | AUTO_AUTO_HEAL / UNK | AutoAutoHeal flag (also seen as 1 during loop) |
| 0x582C | ptr | UNK_582C | Pointer (0x01B784D8 observed) |
| 0x5834 | char[76] | PATH_FILE_NAME | **WRITABLE** — current .mp filename (e.g. "DCCWLOOP.mp") null-padded |
| 0x5880 | u32 | PATH_WP_0 | **WRITABLE** — waypoint checksum 0 (from .mp header field 0) |
| 0x5884 | u32 | PATH_WP_1 | **WRITABLE** — waypoint checksum 1 (from .mp header field 1) |
| 0x5888 | u32 | PATH_WP_2 | Observed 0x20000000 — meaning unknown |
| 0x588C | u32 | PATH_WP_3 | 0 |
| 0x5890 | i32 | PATH_SENTINEL | -1 (0xFFFFFFFF) |
| 0x5894 | i32 | PATH_TOTAL_STEPS | **WRITABLE** — total steps in path (e.g. 2 for DCCWLOOP) |
| 0x5898 | i32 | CUR_PATH_STEP | **WRITABLE** — current step index (1-based, cycles 1→N→1 in loop) |
| 0x589C | i32 | UNK_589C | Observed 18 (0x12) |
| 0x58A0 | i32 | UNK_58A0 | Observed 9 |
| 0x58A4 | i32 | UNK_58A4 | Observed 0x0409 |
| 0x58AC | ptr | UNK_58AC | Pointer (0x01B21778 observed) |

### Session / Combat Statistics

**WARNING:** Session stats (exp meter, accuracy, collected items) are stored in a **heap
allocation** managed by the Player Statistics dialog, NOT in the main game state struct.
The 0x95xx-0x96xx range in the struct contains pointers and stack data, not counters.

**To reset:** Use `SendMessageA(button, BM_CLICK, 0, 0)` on control IDs 1721/1722/1723.
See Player Statistics Window section above and `scripts/reset_stats.py`.

**To read displayed values:** Use `GetWindowTextA` on the static control IDs (1134, 1141, etc.)
from the Player Statistics window via Wine Python.

**Verified reset handler code** (VA 0x470720, uses `eax` = struct base):

The reset function snapshots the current tick and zeros counters at these struct offsets:
- `0x95D8/0x95DC` — session start tick snapshot (copied from 0x9468/0x946C)
- `0x95E0/0x95E4` — second tick snapshot
- `0x95F0-0x9604` — zeroed (internal counters, but NOT the displayed session stats)
- `0x9608-0x960C` — zeroed
- `0x9610/0x9614` — zeroed
- `0x9618` — HP snapshot (copied from 0x53D4)
- `0x961C` — Mana snapshot (copied from 0x53E0)

These are internal bookkeeping values. The actual displayed stats (exp made, duration,
hit%, etc.) are computed and stored in the dialog's own heap memory.

### Timers / Ticks

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x8A98 | i32 | LAG_WAIT_SECONDS | Current lag wait in seconds |
| 0x9468 | i32 | TICK_COUNT_A | Tick/timer value A |
| 0x9508 | i32 | TICK_COUNT_B | Tick/timer value B |

### Misc

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x5A08 | i32 | NO_HANGUP | |

### Global (not struct-relative)

| Address | Size | Name | Description |
|---|---|---|---|
| 0x51F080 | u32 | SECURITY_COOKIE | Stack canary value: 0xBB40E64E |

---

## Memory Write Recipes

### @goto — Walk to a Room (auto-combat ON)

```
1. Write dest room checksum → +0x2EA4 (DEST_CHECKSUM)
2. Write 1                  → +0x564C (GO_FLAG)
3. Write 1                  → +0x5664 (PATHING_ACTIVE)
```

Auto-combat stays on (4D00 unchanged) — will fight monsters along the way.

### @run — Run to a Room (auto-combat OFF)

```
1. Write 0                  → +0x4D00 (AUTO_COMBAT_TOGGLE — combat off)
2. Write dest room checksum → +0x2EA4 (DEST_CHECKSUM)
3. Write 1                  → +0x564C (GO_FLAG)
4. Write 1                  → +0x5664 (PATHING_ACTIVE)
```

MegaMUD will re-enable auto-combat (4D00=1) automatically on arrival.

### Common behavior (walk & run)

MegaMUD will:
- Look up the route from current room to destination
- Chain multiple path files (e.g. GRVB→SRST→GSNE→STSQ→SOVN)
- Walk step by step, handling doors/obstacles
- On arrival: clear DEST_CHECKSUM to 0, GO_FLAG=0, PATHING_ACTIVE=0, MODE=11 (idle)

Room checksums come from `MegaMUD/Default/Rooms.md` (see Room DB section below).

### @loop — Start a Grind Loop (VERIFIED WORKING)

Load path data from .mp file into memory and activate loop mode.
Must be at or near the loop's starting room.

```
1. Write .mp filename     → +0x5834 (PATH_FILE_NAME, null-padded to 76 bytes)
2. Write from_checksum    → +0x5880 (PATH_WP_0, from .mp header field 0)
3. Write to_checksum      → +0x5884 (PATH_WP_1, from .mp header field 1)
4. Write dest_checksum    → +0x2EA4 (DEST_CHECKSUM, same as from_checksum for loops)
5. Write total_steps      → +0x5894 (PATH_TOTAL_STEPS, from .mp header field 2)
6. Write 1                → +0x5898 (CUR_PATH_STEP, start at step 1)
7. Write total_steps      → +0x54D8 (STEPS_REMAINING)
8. Write 0                → +0x54DC (clear sentinel)
9. Write 14               → +0x54BC (MODE = walking)
10. Write 1               → +0x5668 (LOOPING)
11. Write 1               → +0x564C (GO_FLAG)
12. Write 1               → +0x5664 (PATHING_ACTIVE)
```

MegaMUD will:
- Read the .mp file from disk for step directions
- Walk the route step by step
- On reaching the last step, wrap back to step 1 (because LOOPING=1)
- Continue until stopped

### Stop Pathing/Looping (mimics stop button)

```
1. Write 0 → +0x564C (GO_FLAG)
2. Write 0 → +0x5664 (PATHING_ACTIVE)
```

Note: Do NOT clear LOOPING, path file, waypoints, or step data — MegaMUD
manages those internally. Only clear the two flags the stop button uses.

---

## Room Checksum Database (Rooms.md)

**File:** `MegaMUD/Chars/All/Rooms.md` (custom rooms) → fallback `MegaMUD/Default/Rooms.md`

**Format:** colon-separated fields, one room per line:
```
D4E00091:00000004:0:0:0:SBNK:Silvermere:Bank of Godfrey
│         │        │ │ │ │     │          └─ field[7]: room name
│         │        │ │ │ │     └─ field[6]: area name
│         │        │ │ │ └─ field[5]: 4-char room code
│         │        │ │ └─ field[4]: ?
│         │        │ └─ field[3]: ?
│         │        └─ field[2]: ?
│         └─ field[1]: ? (flags?)
└─ field[0]: hex room checksum
```

- 775+ rooms in default DB
- Custom rooms can be added by users
- Checksums are used throughout MegaMUD for room identification

---

## Path File Format (.mp)

**Location:** `MegaMUD/Chars/All/*.mp` (custom) → fallback `MegaMUD/Default/*.mp`

### Path (A → B, one-way)

```
[][]                                        ← empty name (not a loop)
[ARNA:Silvermere:Arena (Practice Dummy)]    ← start: CODE:AREA:ROOM
[STSQ:Silvermere:Town Square]               ← end:   CODE:AREA:ROOM (TWO headers = path)
67110001:9D400055:10:-1:0:::               ← from_cksum:to_cksum:steps:-1:0:::
67110001:0000:n                             ← step: room_cksum:flags:direction
EFF00014:0000:e
DFB40044:0000:s
...
```

### Loop (circular, repeats forever)

```
[CaveWorm][]                                ← loop name in first bracket (has name = loop)
[DCCW:Sewers:Dark Cave]                     ← ONE header only (start = end)
01E00010:01E00010:2:-1:0:::                ← from_cksum == to_cksum (loops back!)
01E00010:0000:e                             ← step 1: walk east
01E01040:0004:w                             ← step 2: walk west (back to start)
```

### Key Differences

| Feature | Path | Loop |
|---|---|---|
| First line | `[][]` (empty) | `[LoopName][]` (has name) |
| Location headers | TWO `[CODE:Area:Room]` | ONE `[CODE:Area:Room]` |
| Header checksums | `from != to` | `from == to` |
| Memory LOOPING flag | 0 | 1 |
| Behavior | Walk A→B, stop | Walk route, wrap to start, repeat |

### Header Format

```
from_checksum:to_checksum:total_steps:-1:0:::
```

- `from_checksum`: room checksum of start room (hex, no 0x prefix)
- `to_checksum`: room checksum of end room (same as from for loops)
- `total_steps`: number of step lines that follow
- `-1:0:::` appears constant

### Step Format

```
room_checksum:flags:direction
```

- `room_checksum`: checksum of room at this step (hex)
- `flags`: usually `0000`, sometimes `0004` — meaning TBD
- `direction`: single char — `n`, `s`, `e`, `w`, or other exit command

---

## Observed Loop Cycle (Cave Worm — DCCWLOOP.mp)

2-room loop, instant regen mob. Full cycle takes ~5-6 seconds.

```
Time     Event
──────── ──────────────────────────────────────────
:00      Room 0x01E00010 (Dark Cave), entities=5
         IN_COMBAT=1, unk_569C=1 (attacking cave worm)
:01      entity_count drops (worm dies)
         IN_COMBAT=0, unk_569C=0
:02      cur_path_step: 2→1 (start walking)
         room_checksum flips to 0x01E01040 (adjacent room)
         cur_path_step: 1→2 (arrived)
:03      entity_count=0 (empty room, just passing through)
:04      room_checksum flips back to 0x01E00010
         entities=5 again (worm has respawned)
         IN_COMBAT=1 (engaging immediately)
:05      cycle repeats
```

**Fields that stay constant during loop:**
- PATHING_ACTIVE = 1 (always on)
- LOOPING = 1 (always on)
- ON_ENTRY_ACTION = 0
- path_file = "DCCWLOOP.mp"
- dest_checksum = 0x01E00010

**Fields that change each cycle:**
- room_checksum (alternates between 2 rooms)
- cur_path_step (alternates 1↔2)
- IN_COMBAT (1 when fighting, 0 when walking)
- unk_569C (tracks IN_COMBAT closely — likely IS_ATTACKING)
- entity_count (drops as mobs die, jumps on room entry)
- cur_mana (fluctuates as spells are cast / regen)

---

## Files

| File | Purpose |
|---|---|
| `~/AI/mudproxy/mudproxy/mem_reader.py` | Memory reader/writer, dual platform (Windows API + Linux /proc) |
| `~/AI/mudproxy/mudproxy/megamud_offsets.py` | All known struct offsets as Python constants |
| `~/AI/mudproxy/mudproxy-web/orchestrator.py` | Game orchestrator, mem_reader integration, WebSocket broadcast |
| `~/AI/mudproxy/mudproxy-web/server.py` | FastAPI server with /api/mem/* endpoints |
| `~/AI/mudproxy/test_memread.py` | Standalone test (--watch, --monitor, --raw, --scan, --name) |
| `MegaMUD/Default/Rooms.md` | Room checksum database (775+ rooms) |
| `MegaMUD/Default/*.mp` | Path/loop files |

## API Endpoints

| Method | Path | Description |
|---|---|---|
| GET | /api/mem/state | Full memory state dump |
| GET | /api/mem/hp | HP and mana |
| GET | /api/mem/stats | All 15 player stats |
| GET | /api/mem/flags | 23 status flags |
| GET | /api/mem/room | Room name, checksum, darkness |
| GET | /api/mem/party | Party members with HP |
| GET | /api/mem/player | Name, level, race, class |
| GET | /api/mem/exp | 64-bit experience values |
| POST | /api/mem/attach | Manual re-attach trigger |
| POST | /api/mem/goto | Navigate to room by name/code/partial match |
| GET | /api/mem/rooms?q= | Search room database |
| WS | mem_state | Broadcasts state changes in real-time |

## TODO

- [x] Implement @loop via memory writes — VERIFIED WORKING
- [x] Map all 11 automation toggles (0x4D00–0x4D28) — DONE
- [x] Map default profiles 1 & 2 (0x4D2C–0x4D80) — DONE
- [ ] Parse .mp files to extract loop/path data for the API
- [ ] Add POST /api/mem/loop endpoint
- [ ] Add POST /api/mem/stop endpoint
- [ ] Add POST /api/mem/toggles endpoint (read/write all 11 switches)
- [ ] Tie goto/loop/toggles into web UI
- [ ] Map DisableEvents, EventsAfkOnly, AutoConnect offsets (interactive scan needed)
- [ ] Map health threshold offsets (HpFull%, HpRest%, HpHeal%, etc.)
- [ ] Map unk_569C definitively (IS_ATTACKING?)
- [ ] Map unk_5820, unk_589C, unk_58A0, unk_58A4
- [ ] Explore spell queue / auto-combat decision structures
- [ ] Map step flags field in .mp files (0000 vs 0004)
- [ ] Build scripting/scheduling system for automated event chains
