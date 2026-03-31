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
| 0x54BC | i32 | MODE | 11=idle, 14=walking/running, 15=looping, 17=lost (re-syncing) |
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
9. Write 1                → +0x54B4 (ON_ENTRY_ACTION = resume loop on arrival)
10. Write 15              → +0x54BC (MODE = looping; 14=walking, 15=looping)
11. Write 1               → +0x5668 (LOOPING)
12. Write 1               → +0x564C (GO_FLAG)
13. Write 1               → +0x5664 (PATHING_ACTIVE)
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
- [ ] Fully reverse engineer Players.md gender/appearance fields
- [ ] Map all MDB2 record field offsets for Players, Items, Monsters, Rooms

## MDB2 File Format (C-Index/II Database)

MegaMUD uses a proprietary binary database format with the magic header `MDB2`.
This is NOT Microsoft Access MDB, NOT Btrieve — it's MegaMUD's own C-Index/II engine.

### File Locations

| File | Location | Format | Purpose |
|---|---|---|---|
| Rooms.md | `Default/` | Text (NOT MDB2) | Room checksums, names, exits |
| Items.md | `Default/` | MDB2 | Item database |
| Monsters.md | `Default/` | MDB2 | Monster/NPC database |
| Spells.md | `Default/` | MDB2 | Spell database |
| Classes.md | `Default/` | MDB2 | Class definitions |
| Races.md | `Default/` | MDB2 | Race definitions |
| Paths.md | `Default/` | MDB2 | Path/waypoint data |
| Macros.md | `Default/` | Text (NOT MDB2) | Macro definitions |
| Messages.md | `Default/` | Text (NOT MDB2) | Message templates |
| Players.md | `BBS/<bbsname>/` | MDB2 | Per-BBS player database (inventories, stats) |

### Page Structure

All MDB2 files use a fixed **page size of 0x400 (1024) bytes**.
Page 0 is the file header. Pages 1+ are either **data leaf pages** or **B-tree index pages**.

### MDB2 Header (Page 0, 0x400 bytes)

```
Offset  Type    Field
0x00    char[4] Magic: "MDB2"
0x04    u16     Version/type (varies per file: 2 for simple, 52-85 for complex)
0x06    u16     Key size or field count (0x32=50 common for Items/Monsters/Players)
0x08    u32     Unknown (checksum or data size descriptor)
0x0C    u16     Number of active data records
0x0E    u16     Unknown flags
0x10    u32     Always 1
0x14    u16     Total data pages = (file_size / 0x400) - 1  ← VERIFIED
0x16    u16     Unknown (often mirrors 0x06)
0x18    u16     Unknown (NOT record size — misleading in complex files)
0x1A    u16     Unknown
0x1C-   zeros   Padding to 0x400
```

**Verified across all files:** `u16 at 0x14` = total data pages = `(file_size / 1024) - 1`.

### Data Page Header (12 bytes)

Each data page begins with a 12-byte header:

```
Offset  Type  Field
+0x00   u16   Page type: 0x0000 = data leaf, 0x0001 = B-tree index
+0x02   u16   Entry count in this page (e.g. 6 = 3 records for Players.md)
+0x04   u16   Free space offset or size indicator
+0x06   u16   Always 0
+0x08   u16   Page sequence number
+0x0A   u16   Previous/next page pointer (linked list, 0xFFFF = none)
```

Data records begin at offset +0x0C within the page (immediately after the header).

### B-tree Index Pages

Pages with type `0x0001` are B-tree internal nodes. They contain sorted lists of
player name strings used for key lookup. Index pages do NOT contain `0xFE 0x01`
data markers — they only hold name references for tree traversal.

### Players.md Record Structure

Player records are identified by a `0xFE 0x01` marker. Each data page typically
holds 1-3 player records. Records are **variable-length** due to the first name
field, but all subsequent fields are at **fixed offsets from `end_of_name1`**.

#### Variable-Length Key: Name Field 1

```
+0x00   2      Marker: 0xFE 0x01
+0x02   var    Name1: null-terminated ASCII + 5 padding bytes
               Field size = strlen(name) + 6
               Example: "Tripmunk" = 8 + 6 = 14 bytes
```

**CRITICAL:** `end_of_name1 = 0x02 + strlen(name) + 6`. ALL subsequent field
offsets are relative to this value. A 5-char name shifts everything 3 bytes
earlier than an 8-char name.

#### Fixed Fields (relative to `end_of_name1`)

```
Offset  Size  Type   Field
+0x00   11    str    Name2 (display name copy, fixed 11 bytes = 10 chars max + null)
+0x0B   50    str    Surname/Title (fixed 50-byte buffer, null-terminated within)
+0x3D   var   str    Gang/Guild name (null-terminated within remaining buffer space)

+0x52   2     u16    Record type (4=has inventory+stats, 5=minimal, 6=partial, 7=WHO-only)
+0x54   2     u16    Flags (0x1000 = standard, 0x1002 = has inventory data)
+0x56   2     u16    Race ID (0=unknown, 1-13 = MajorMUD race)
+0x58   2     u16    Class ID (0=unknown, 1-13 = MajorMUD class)
+0x5A   2     u16    Unknown field (values 3-9)
+0x5C   2     u16    Unknown field (0 or 2)
+0x5E   2     u16    Level
+0x60   12    u16x6  Stats: STR, INT, WIL, AGI, HEA, CHA (values 0-110)
+0x6C   16    -      Padding (zeros)

+0x7C   4     u32    Timestamp 1 — Unix epoch (first seen or last WHO update)
+0x80   4     -      Padding (zeros)
+0x84   4     u32    Timestamp 2 — Unix epoch (last inventory/look update, 0 if never)
+0x88   12    -      Padding (zeros)

+0x94   8     -      Padding (zeros)
+0x9C   var   u16[]  Equipment item IDs (zero-terminated list, references Items.md)
```

#### Race ID Mapping

| ID | Race | ID | Race |
|---|---|---|---|
| 0 | Unknown | 7 | Dark-Elf |
| 1 | Human | 8 | Half-Orc |
| 2 | Dwarf | 9 | Goblin |
| 3 | Gnome | 10 | Half-Ogre |
| 4 | Halfling | 11 | Kang |
| 5 | Elf | 12 | Nekojin |
| 6 | Half-Elf | 13 | Gaunt One |

#### Class ID Mapping

| ID | Class | ID | Class |
|---|---|---|---|
| 0 | Unknown | 7 | Bard |
| 1 | Warrior | 8 | Missionary |
| 2 | Witchunter | 9 | Ranger |
| 3 | Cleric | 10 | Paladin |
| 4 | Thief | 11 | Warlock |
| 5 | Mage | 12 | Ninja |
| 6 | Ganbusher | 13 | Druid |

#### Verified Player Data Examples

| Player | Race | Class | Level | Stats (S/I/W/A/H/C) | Items |
|---|---|---|---|---|---|
| Tripmunk | 4 Halfling | 9 Ranger | 16 | 60/90/50/40/50/70 | 18 items |
| Amadeus | 10 Half-Ogre | 11 Warlock | 40 | 110/100/80/1/100/1 | 14 items |
| Aries | 8 Half-Orc | 7 Bard | 30 | 90/90/60/40/90/80 | 12 items |
| Cidir | 12 Nekojin | 9 Ranger | 50 | 110/100/40/30/70/100 | 6 items |
| Francisco | 8 Half-Orc | 9 Ranger | 54 | 110/100/60/50/100/100 | 5 items |

#### Data Completeness Signals

- **race=0, class=0, level=1, all stats=0:** Never seen in WHO — truly unknown player
- **race>0, class>0, level>0, stats=0:** Seen in WHO list but never LOOKed at
- **flags=0x1002 + non-zero equipment:** Has inventory data from a LOOK command
- **Default display:** MegaMUD shows race=7 (Dark-Elf), class=10 (Paladin) when
  displaying unknown players — but stores 0/0 in the database. Dark-Elf Paladin
  is an intentionally terrible build choice, so seeing it is a strong signal
  that real data hasn't been collected.

**NOT stored in Players.md:** Gender, hair color, eye color, hair length.
Appearance data must come from parsing the BBS `look` output.

### Items.md Record Structure

**File:** `Default/Items.md` — 912 pages, 1,938 items
**No FE01 markers** — records start directly with `[length_byte] 01 [key_string...]`

#### Record Header (variable length)

```
Offset  Size  Field
+0x00   1     Record length (total = length_byte + 1)
+0x01   1     0x01 marker
+0x02   var   Key string (item number as ASCII, null-terminated)
        5     Null padding (terminator + 4 extra nulls)
        1     0x80 marker (end of key region)
        2     u16 LE item number (binary)
```

Total header = `strlen(key) + 10` bytes. After header: **198-byte fixed payload**.

#### Payload: String Region (0x00–0x58, 89 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 30 | Item name (null-terminated, junk after null) |
| 0x1E | 59 | Shop/location name where sold (null-terminated) |

#### Payload: Numeric Region (0x59–0xC5, 109 bytes)

All offsets relative to numeric region start (payload + 0x59).

| Offset | Size | Field | Confidence |
|--------|------|-------|------------|
| +0x00 | u8 | **Item Type** (see type table below) | HIGH |
| +0x02 | u16 | **Weight** (in tenths of a stone) | HIGH |
| +0x04 | u32 | **Base Value** (sell price in copper) | HIGH |
| +0x08 | u16 | **Flags 1** (0x0200 = magical) | MEDIUM |
| +0x0A | u16 | **Flags 2** (0x4002 = quest item?) | LOW |
| +0x0C | u16 | Unknown (usually 0 or 1) | LOW |
| +0x0E | i16 | **Charges** (-1 = unlimited/permanent, 1 = single-use) | HIGH |
| +0x10 | u16 | **Strength Requirement** | HIGH |
| +0x12 | u16 | **Number of Damage Dice** | HIGH |
| +0x14 | u16 | **Damage Die Size** (e.g. 3d12 = dice=3, size=12) | HIGH |
| +0x16 | i16 | **Accuracy Modifier** (signed, neg = clumsy) | HIGH |
| +0x18 | u16 | **Speed** (weapons: 3–11, armor: 32 standard) | HIGH |
| +0x1A | u16 | **AC (Armor Class)** for armor | HIGH |
| +0x1C | u16 | **DR (Damage Resistance)** for armor (0–100) | HIGH |
| +0x1E | u16 | **Class Restriction ID 1** | MEDIUM |
| +0x20 | u16 | **Class Restriction ID 2** | MEDIUM |
| +0x22–+0x2A | u16[5] | Additional restriction/ability IDs | LOW |
| +0x32 | i16 | Signed modifier (magical bonus?) | MEDIUM |
| +0x34–+0x46 | 18 | Stat/ability modifier pairs | MEDIUM |
| +0x60 | u32 | **Shop Buy Price** (42–128× base value) | HIGH |

#### Item Type Values

| Value | Type | Count |
|-------|------|-------|
| 0 | Armor/Clothing | 515 |
| 1 | Weapon | 384 |
| 3 | Edible/Usable | 488 |
| 4 | Food? | 15 |
| 5 | Light/Torch | 33 |
| 7 | Key/Container | 88 |
| 8 | Thrown/Ammo | 27 |
| 9 | Scroll | 223 |
| 10 | Potion/Drink | 150 |

#### Verification

| Item | # | Type | Weight | Value | Dice | Str | Speed | AC | DR |
|------|---|------|--------|-------|------|-----|-------|----|----|
| Longsword | 64 | 1 | 90 | 2400 | 3d12 | 45 | 5 | — | — |
| Full Plate Corselet | 19 | 0 | 1500 | 50000 | — | — | — | 230 | 100 |
| Scroll of Agony | 140 | 9 | 10 | 18750 | — | — | — | — | — |
| Phoenix Feather | 1000 | 0 | 0 | 0 | — | — | — | — | — |

### Races.md Record Structure

**File:** `Default/Races.md` — 7 pages, 13 races
**Record header:** Same format as Items — `[length] 01 [key] [nullpad] 80 [u16_id] [payload]`
**Payload:** 85 bytes fixed per race

#### Payload Fields

| Offset | Size | Field | Verified |
|--------|------|-------|----------|
| +0x00 | 30 | Race name (null-terminated) | ✓ Human, Dwarf, Elf, Kang, etc. |
| +0x1E | 6×u8 | **Min Stats**: STR, INT, WIL, AGI, HLT, CHM | ✓ Human=all 40, Half-Ogre=70/20/20/25/60/25 |
| +0x24 | 6×u8 | **Max Stats**: STR, INT, WIL, AGI, HLT, CHM | ✓ Human=all 100, Half-Ogre=150/60/60/70/150/60 |
| +0x2A | u8 | Flag (0xFF for Halfling, else 0x00) | ✓ |
| +0x2B | u16 | **Magic Resistance** | ✓ Elf=45, Dwarf=30, Human=0 |
| +0x2D | u16 | Racial ability ID 1 | — |
| +0x2F | u16 | Racial ability ID 2 | — |
| +0x31 | u16 | Racial ability ID 3 | — |
| +0x33–0x40 | var | Additional ability data (sparse) | — |
| +0x41–0x54 | var | Racial bonuses/modifiers (sparse) | — |

#### Verified Race Data

| # | Race | MinSTR | MaxSTR | MinINT | MaxINT | MR |
|---|------|--------|--------|--------|--------|----|
| 1 | Human | 40 | 100 | 40 | 100 | 0 |
| 2 | Dwarf | 50 | 110 | 30 | 90 | 30 |
| 5 | Elf | 35 | 90 | 50 | 120 | 45 |
| 10 | Half-Ogre | 70 | 150 | 20 | 60 | 30 |
| 11 | Kang | 55 | 120 | 30 | 90 | 35 |
| 12 | Nekojin | 40 | 100 | 60 | 130 | 50 |
| 13 | Gaunt One | 40 | 100 | 50 | 110 | 50 |

### Classes.md Record Structure

**File:** `Default/Classes.md` — 7 pages, 15 classes
**Record header:** Same format as Items
**Payload:** 79 bytes fixed per class

#### Payload Fields

| Offset | Size | Field | Verified |
|--------|------|-------|----------|
| +0x00 | 30 | Class name (null-terminated) | ✓ Warrior, Mage, Thief, etc. |
| +0x1E | u16 | **Experience %** (100=normal, higher=harder) | ✓ Warrior=100, Thief=80, Ranger=250 |
| +0x20 | u8 | **HP Per Level** | ✓ Warrior=6, Mage=3, Ranger=6 |
| +0x21 | u8 | Unknown (secondary stat per level?) | — |
| +0x22 | u8 | **Combat Level** (affects to-hit) | ✓ Warrior=10, Mage=6, Ninja=8 |
| +0x23 | u8 | **Mana Per Level** | ✓ Mage=9, Priest=7, Warrior=8? |
| +0x24 | u8 | Unknown | — |
| +0x25 | u8 | Starting weapon/armor type 1 | — |
| +0x26 | u8 | Starting weapon/armor type 2 | — |
| +0x27 | u16[] | **Ability IDs** (variable-length list) | — |
| ... | u16 | 0xFF9D (-99) sentinel = empty slot | ✓ |
| +0x4E | u8 | End padding / trailing byte (0x20 = space) | ✓ |

#### Verified Class Data

| # | Class | Exp% | HP/Lvl | Combat | Abilities |
|---|-------|------|--------|--------|-----------|
| 1 | Warrior | 100 | 6 | 10 | basic combat |
| 2 | Witchunter | 130 | 7 | 10 | MR+51, detect |
| 5 | Priest | 140 | 3 | 6 | healing spells |
| 7 | Ninja | 185 | 5 | 8 | stealth, combat |
| 8 | Thief | 80 | 4 | 7 | lockpick, stealth |
| 12 | Mage | 140 | 3 | 6 | offensive magic |
| 14 | Ranger | 250 | 6 | 9 | nature + combat |
| 15 | Mystic | 250 | 5 | 8 | martial arts + magic |

### Monsters.md Record Structure

**File:** `Default/Monsters.md` — 529 pages, 1,128 monsters
**Record header:** Same format as Items
**Payload:** 209 bytes fixed per monster

#### Payload Fields

Cross-referenced against MME (GMUD Explorer v2.1.1) for hydra(#589) and dwarven cleric(#417).

| Offset | Size | Field | Confidence |
|--------|------|-------|------------|
| +0x00 | 30 | Monster name (null-terminated) | HIGH |
| +0x1E | u8 | Unknown flag | LOW |
| +0x1F | u8 | NPC flag (0x40 = NPC/humanoid) | MEDIUM |
| +0x20 | u8 | Unknown | LOW |
| +0x21 | u8 | Unknown | LOW |
| +0x22 | u16 | **Type flags** (0x0440=combat mob, 0x0240=NPC, 0x0340=special) | MEDIUM |
| +0x24–0x31 | 14 | Unknown/padding | LOW |
| +0x32 | u8 | **Alignment** (0=neutral, 1=evil, 2=good) | MEDIUM |
| +0x33 | u16 | **HP Regen** (HP restored per tick, ~30 sec) | HIGH ✓ hydra=360 (20% of 1800), cleric=20 (10% of 200) |
| +0x35–0x36 | 2 | Padding/zeros | — |
| +0x37 | u16 | **Hit Points** | HIGH ✓ hydra=1800, cleric=200 |
| +0x39 | u16 | **Energy/Round** (usually 1000, 0 for invulnerable NPCs) | HIGH ✓ |
| +0x3B | u16 | **Magic Resistance** | HIGH ✓ hydra=100, cleric=60 |
| +0x3D | u16 | **Follow %** | HIGH ✓ hydra=100, cleric=70 |
| +0x3F | u16 | **Armor Class** | HIGH ✓ hydra=65, cleric=50 |
| +0x41 | u16 | **Damage Resistance** | HIGH ✓ hydra=20, cleric=3 |
| +0x43 | u16 | **Charm Level** (999 = uncharmable) | HIGH ✓ hydra=999, cleric=40 |
| +0x45 | u16 | **BS Defence** (backstab defense) | MEDIUM — hydra=20, cleric=24 |
| +0x47 | u8 | Unknown (0-3) | LOW |
| +0x48 | u8 | **Number of Attacks** (0=default 1 attack) | HIGH ✓ hydra=2, cleric=0(=1) |
| +0x49 | u16 | **Game Limit** (max active in realm, 0=unlimited) | HIGH ✓ hydra=1, cleric=5 |
| +0x4B–0x50 | 6 | **Spell/ability data** (see below) | MEDIUM |
| +0x51–0x5A | 10 | **Item Drop IDs** (5 × u16 LE, references Items.md) | HIGH ✓ |
| +0x5B–0x5F | 5 | **Drop Percentages** (5 × u8, 1-100% per item slot) | HIGH ✓ |
| +0x60 | u16 | **Cash: Runic** (coin drops) | HIGH ✓ |
| +0x62 | u16 | **Cash: Platinum** | HIGH ✓ |
| +0x64 | u16 | **Cash: Gold** | HIGH ✓ cleric=10 (MME: "Cash up to 10 Gold") |
| +0x66 | u16 | **Cash: Silver** | HIGH ✓ |
| +0x68 | u16 | **Cash: Copper** | HIGH ✓ |
| +0x6A–0x72 | 10 | **Ability Types [0–4]** (5 × u16, see ability table) | HIGH ✓ |
| +0x74–0x7C | 10 | **Ability Params [0–4]** (5 × u16, paired with types) | HIGH ✓ |
| +0x7E | u16 | **Weapon Number** (references Items.md, 0=natural) | HIGH ✓ cleric=869 (dwarven hammer) |
| +0x80–0x88 | 10 | **Between-Rounds Spell IDs [0–4]** (5 × u16, refs Spells.md) | HIGH ✓ |
| +0x8A–0x91 | 8 | **Extended spell/hit data** (partially decoded) | LOW |
| +0x92 | u32 | **Experience** | HIGH ✓ cleric=150, hydra=1,350,000 |
| +0x96–0x9A | 5 | **Between-Rounds Cast % [0–4]** (u8 cumulative, see section) | HIGH ✓ |
| +0x9B–0x9D | 3 | **Between-Rounds Cast Level [0–2]** (u8, spell level) | MEDIUM |
| +0x9E | u16 | **Attack Min Damage [0]** (editable copy, 95% matches WCC) | HIGH ✓ hydra=100, cleric=4 |
| +0xA0 | u16 | **Attack Min Damage [0]** (baseline copy, 99% matches WCC) | HIGH ✓ |
| +0xA2 | u16 | **Attack Min Damage [1]** | HIGH ✓ hydra=50 |
| +0xA4 | u16 | **Attack Min Damage [2]** | HIGH ✓ |
| +0xA6 | u16 | **Attack Min Damage [3]** | HIGH ✓ |
| +0xA8 | u16 | **Attack Min Damage [4]** | HIGH ✓ |
| +0xAA | u16 | **Attack Max Damage [0]** | HIGH ✓ hydra=250, cleric=16 |
| +0xAC | u16 | **Attack Max Damage [1]** | HIGH ✓ hydra=100 |
| +0xAE | u16 | **Attack Max Damage [2]** | HIGH ✓ |
| +0xB0 | u16 | **Attack Max Damage [3]** | HIGH ✓ |
| +0xB2 | u16 | **Attack Max Damage [4]** | HIGH ✓ |
| +0xB4 | u16 | **Attack Energy [0]** (cost per swing, out of 1000/round) | HIGH ✓ hydra=500, cleric=333 |
| +0xB6 | u16 | **Attack Energy [1]** | HIGH ✓ hydra=500 |
| +0xB8 | u16 | **Attack Energy [2]** | HIGH ✓ |
| +0xBA | u16 | **Attack Energy [3]** | HIGH ✓ |
| +0xBC | u16 | **Attack Energy [4]** | HIGH ✓ |
| +0xBE–0xBF | 2 | Unknown | LOW |
| +0xC0 | u16 | **Create Spell** (spell cast on spawn/creation) | MEDIUM ✓ hydra=90 (hydra head create) |
| +0xC2–0xC6 | 5 | Unknown | LOW |
| +0xC7 | u8 | **Undead Flag** (1=undead, 0=living) | HIGH ✓ |
| +0xC8 | u16 | **Attack Hit Spell** (spell cast on successful melee hit) | MEDIUM |
| +0xCA–0xCD | 4 | Padding | — |
| +0xCE | u16 | Unknown (0–9999, possibly GroupExp or CombatValue) | LOW |

**Note:** MDB2 stores 5 attack slots (matching WCC) and 5 item drop slots (WCC supports 10 drops).

#### Ability Array (+0x6A–0x7C) — DECODED

5 paired slots: type at +0x6A+n*2, param at +0x74+n*2. Types from WCC AbilityA/AbilityB
system, names confirmed from MME source (`GetAbilityName()` in modMMudFunc.bas):

| Type | Name | Param meaning | Freq |
|------|------|---------------|------|
| 3 | Resist-Cold | +N resistance | 155 |
| 5 | Resist-Fire | +N resistance | 174 |
| 9 | Shadow | Flag (+10 AC flat, single source, doesn't stack) | — |
| 12 | Summon | Monster ID to summon | — |
| 21 | ImmuPoison | Flag | 243 |
| 28 | Magical | +N (weapon enchantment level to hit) | 376 |
| 34 | Dodge | +N dodge bonus | 169 |
| 57 | SeeHidden | Flag (sees sneaking/hidden players) | 195 |
| 65 | Resist-Stone | +N resistance | — |
| 66 | Resist-Lightning | +N resistance | — |
| 78 | Animal | Flag (affected by animal-targeting spells) | 175 |
| 109 | NonLiving | Flag (immune to living-only effects) | 189 |
| 139 | SpellImmu | +N spell immunity rating | 398 |
| **146** | **MonsGuards** | **Monster ID that guards this monster** | **161** |
| 147 | Resist-Water | +N resistance | — |
| 185 | NoAttackIfItemNum | Item ID (won't attack if player holds item) | — |

**Verified ability examples:**
- hydra(#589): Magical +4, SpellImmu +20, MonsGuards hydra head(590)
- dwarven cleric(#417): MonsGuards dwarven guard(396)
- minotaur champion(#60): MonsGuards minotaur(111)
- werewolf(#127): MonsGuards dire wolf(56)
- ice sorceress(#108): MonsGuards ice golem(211)
- King Kulgar(#430): 5 guards — dwarven royal guard(426)×2, warrior(418), guard(396), cleric(417)

#### Between-Rounds Spells (+0x80–0x88) and Cast Data (+0x96–0x9D)

5 spell ID slots (u16 each) for abilities cast between combat rounds.

**Cast percentages** at +0x96–0x9A (5 × u8) are **cumulative**:
each byte = sum of all prior slots + this slot's individual chance.
To get individual: `slot[n] - slot[n-1]` (slot[-1] = 0).

**Cast levels** at +0x9B–0x9D (3 × u8) — spell level for the first 3 slots.

**Verified from MME:**
- dwarven cleric(#417): blind(77) 20%, hold person(66) 20%, summon guard(510) 40%
  - +0x96 bytes: 20, 40, 80 → individual 20%, 20%, 40% ✓
- hydra(#589): hydra head(726) 20%
  - +0x96 bytes: 20 → individual 20% ✓

#### Spell/Ability Data (+0x4B–0x50)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| +0x4B | u16 | Spell-related field | TBD — may be cast chance or ability count |
| +0x4D | u16 | Spell-related field | TBD — may be ability category (0-17) |
| +0x4F | u16 | **Spell ID** | References Spells.md (verified for many monsters) |

**Verified spell references:**
- Sheriff Lionheart (+0x4F=102) → "song of blasting"
- high druid (+0x4F=142) → "poison cloud"
- quickling lord (+0x4F=254) → "colour spray"
- guardian beast (+0x4F=706) → "forked lightning"
- gaunt one temple master (+0x4F=1362) → "partial petrification"
- Timelord (+0x4F=1041) → "bladed sphere"

#### Item Drop Slots (+0x51–0x5F)

5 equipment/loot slots, each a u16 item ID referencing Items.md, followed by 5 u8 drop percentages:

**Verified item drops:**
- Sheriff Lionheart: gavel of justice (1348, 1%), jail key (1416, 1%), golden braided belt (167, 2%)
- Giant spider: item 216 (10%), spider leg (1273, 5%)
- Guardian golem: platinum bracers (215, 5%), black leather tunic (213, 5%), + 3 more items (5% each)
- Mummy: iron ring (161, 1%), item 162 (1%), silver ring (163, 1%), item 164 (1%), item 177 (100%)

#### Verified Monster Data (MME cross-reference)

| Monster | # | HP | AC | DR | MR | Follow% | Charm | Limit | Weapon | EXP |
|---------|---|----|----|----|----|---------| ------|-------|--------|-----|
| dwarven cleric | 417 | 200 | 50 | 3 | 60 | 70 | 40 | 5 | 869 (dwarven hammer) | 150 |
| hydra | 589 | 1800 | 65 | 20 | 100 | 100 | 999 | 1 | 0 (natural) | 2,000,000 |

#### NPC Detection

Invulnerable NPCs have Energy=0, AC=65526 (0xFFFA), Charm=999:
- Guildmaster (#38), old man (#39), bishop (#45), priest (#46), healer (#47)
- Named NPCs (Aiken, Thuluk, Colin, etc.) have Energy=1000 but EXP=5

### Spells.md Record Structure

**File:** `Default/Spells.md` — 487 pages, 471 spells
**Record header:** Same format as Items
**Payload:** 156 bytes fixed per spell

#### Payload Fields

| Offset | Size | Field | Confidence |
|--------|------|-------|------------|
| +0x00 | 30 | Spell name (null-terminated) | HIGH |
| +0x1E | 7 | **Command abbreviation** (null-terminated, 7-byte buffer) | HIGH ✓ "mmis", "lbol", "heal" |
| +0x25 | u16 | **Mana Cost** (raw value, likely centimana) | HIGH ✓ magic missile=100, heal=56 |
| +0x27 | u16 | **Flags** (0x4000=player-usable, 0x0000=system/monster) | HIGH |
| +0x29–0x51 | 41 | Reserved/extended data (mostly zeros) | LOW |
| +0x52 | u8 | **Level Required** | HIGH ✓ magic missile=1, lightning bolt=8, swarm=12 |
| +0x53 | u8 | **Tier/Power** (0-10, higher for multi-class spells) | MEDIUM |
| +0x54 | u8 | **Duration Base** (0-28, buff duration scaling) | MEDIUM |
| +0x55 | u8 | Padding | — |
| +0x56 | u16 | **Charges** (1000=combat spell, 999=special, 0=non-combat) | MEDIUM |
| +0x58 | u16 | **Min Effect** (damage, heal, or stat boost amount) | HIGH |
| +0x5A | u16 | **Max Effect** | HIGH |
| +0x5C | u16 | **Duration** (0=instant, 40-80 for buffs) | MEDIUM |
| +0x5E | i16 | **Accuracy/Power Modifier** (signed, negative = debuff) | MEDIUM |
| +0x60 | u8 | **Target Type** | HIGH |
| +0x61 | u8 | **Spell School** | HIGH |
| +0x62 | u8 | Unknown (often 4) | LOW |
| +0x63 | u8 | **Effect/Ability ID** (what the spell actually does) | MEDIUM |
| +0x64–0x9B | 56 | Extended effect data, ability arrays | LOW |

#### Target Type Values (+0x60)

| Value | Type | Examples |
|-------|------|---------|
| 1 | Self buff | illuminate, ethereal shield, barkskin |
| 2 | Single target (heal/cure) | minor healing, cure poison |
| 4 | Undead-specific | turn undead |
| 7 | Utility | detect magic |
| 8 | Combat attack (single) | magic missile, lightning bolt |
| 12 | Area of Effect | swarm, mana storm |
| 13 | Party/group buff | holy aura, unholy aura |

#### Spell School Values (+0x61)

| Value | School | Examples |
|-------|--------|---------|
| 1 | Cleric | harm, minor healing, bless, turn undead |
| 3 | Mage (secondary) | — |
| 4 | Mage (primary) | magic missile, frost jet, lightning bolt |
| 5 | Warlock | — |
| 7 | Druid | vine strike, starlight, mend, barkskin |
| 10 | Bard | song of lore, song of valour |
| 11 | Ninja/Martial | way of the swan/tiger/cat/rat |

#### Verified Spell Data

| Spell | # | Cmd | Mana | Lvl | Min | Max | Dur | Target | School |
|-------|---|-----|------|-----|-----|-----|-----|--------|--------|
| magic missile | 1 | mmis | 100 | 1 | 4 | 12 | 0 | attack | mage |
| minor healing | 13 | mihe | 56 | 1 | 2 | 8 | 0 | heal | cleric |
| lightning bolt | 8 | lbol | 100 | 8 | 16 | 12 | 0 | attack | mage |
| bless | 14 | bles | 58 | 2 | 3 | 3 | 40 | buff | cleric |
| holy aura | 20 | aura | 154 | 9 | 10 | 10 | 60 | party | cleric |
| barkskin | 34 | skin | 26 | 7 | 10 | 10 | 40 | self | druid |
| song of valour | 42 | valr | 154 | 2 | 5 | 5 | 35 | party | bard |
| way of the swan | 36 | swan | 24 | 2 | 3 | 3 | 0 | self | martial |

### Paths.md Record Structure

**File:** `Default/Paths.md` — 1,237 pages, 1,146 records (748 full + 398 stubs)
**Record format:** Non-standard — does NOT use the universal 0x80 marker format for full records.

Two record types coexist in the same B-tree:

#### Full Record (length=254, 255 bytes total)

Stores path/loop metadata with routing hashes. The actual step-by-step path
data lives in the corresponding `.mp` file on disk.

| Offset | Size | Field | Confidence |
|--------|------|-------|------------|
| +0x00 | 1 | Length byte (0xFE = 254) | — |
| +0x01 | 1 | 0x01 marker | — |
| +0x02–0x12 | 17 | **Filename** (key, null-padded, e.g. "AADEASYL.mp") | HIGH |
| +0x13–0x4F | 61 | **Loop/Path Display Name** (null-padded, empty for unnamed) | HIGH ✓ |
| +0x50–0x78 | 41 | **Creator Name** (null-padded) | HIGH ✓ |
| +0x79–0xBF | 71 | **Filename** (repeated, null-padded) | HIGH |
| +0xC0–0xC5 | 6 | Reserved/zeros | — |
| +0xC6–0xC9 | u32 | **Start Room Hash** (LE) | HIGH ✓ matches .mp file |
| +0xCA–0xCD | u32 | **End Room Hash** (LE) | HIGH ✓ matches .mp file |
| +0xCE–0xCF | u16 | Flags/options (usually 0, rare values: 5, 7, 9999) | LOW |
| +0xD0–0xD5 | 6 | Sentinel (constant 0x0000FFFFFFFF) | HIGH |
| +0xD6–0xD7 | u16 | **Step Count** (LE) | HIGH ✓ matches .mp file |
| +0xD8–0xFE | 39 | Reserved/zeros | — |

**Loops vs Paths:** When start_hash == end_hash, it's a loop. Otherwise it's a point-to-point path.

#### Stub Record (length=45, 46 bytes total)

Placeholder for `.mp` files that exist on disk but have no routing metadata in the database.
Uses the standard universal record format: `[0x2D] [0x01] [filename] [pad] [0x80] [0x0000] [zeros]`

#### Verified Path Data

| Filename | Name | Creator | Steps | Type |
|----------|------|---------|-------|------|
| slm2loop.mp | Slum loop with HO's in it | — | 82 | Loop |
| BLMOLOOP.mp | Black Mountains Loop | Spork ZSpoon | 277 | Loop |
| HKEPLOOP.mp | Bandit Keep | Winterhawk | 78 | Loop |
| LAVTLOO2.mp | Dragons Loop (larger) | Winterhawk | 129 | Loop |
| STONLOO2.mp | Stone Elementals Regen Fixer loop | Kyrra | 494 | Loop |
| AADEASYL.mp | — | Winterhawk | 3 | Path |

### MDB2 Record Format (Universal)

All MDB2 databases (Items, Monsters, Spells, Races, Classes, Players) share the
same record encoding within data leaf pages:

```
[length_byte] [0x01] [key_ascii] [null] [padding×4] [0x80] [u16_id] [payload]

- length_byte: total record size = length_byte + 1
- 0x01: record marker (always 1)
- key_ascii: record ID as ASCII decimal string (e.g., "1", "10", "1000")
- null + padding: 1 null terminator + 4 zero bytes = 5 total
- 0x80: end-of-key marker
- u16_id: record ID as little-endian 16-bit integer
- payload: fixed-size data block (size varies by database)
```

Total record header = `strlen(key_ascii) + 10` bytes (after length byte).
Records are packed contiguously on data leaf pages starting at page offset +0x0C.

### Database Summary

| Database | Records | Payload | Key Fields Decoded |
|----------|---------|---------|-------------------|
| Items.md | 1,938 | 198 bytes | ✓ Name, type, weight, value, damage, AC, DR, speed, class restrictions |
| Monsters.md | 1,128 | 209 bytes | ✓ Name, HP, AC/DR, MR, Follow%, Charm, GameLimit, attacks, cash, EXP, abilities, guards, weapon, spells, item drops |
| Spells.md | 471 | 156 bytes | ✓ Name, command, mana, level, damage, duration, target, school |
| Races.md | 13 | 85 bytes | ✓ Name, min/max stats, MR |
| Classes.md | 15 | 79 bytes | ✓ Name, exp%, HP/level, combat level, abilities |
| Players.md | var | var | ✓ Name, race, class, level, stats, equipment IDs |
| Paths.md | 1,146 | 254 bytes (full) / 45 bytes (stub) | ✓ Filename, name, creator, hashes, step count |

### Notes

- Rooms.md, Macros.md, and Messages.md are plain text, NOT MDB2 format
- MMUD Explorer reads Rooms.md for path data but uses Access/JET databases
  for everything else — it does NOT parse Players.md directly
- The MDB2 format is undocumented publicly; no known complete specification exists
- Page size 0x400 and B-tree structure are consistent across all MDB2 files
- All record headers follow the universal format: `[len] 01 [key] [pad] 80 [id] [payload]`
- Equipment item IDs in Players.md reference the Items.md database
- Monster record fully mapped from +0x37 to +0xC8 via MME cross-reference:
  - +0x37–0x49: Core stats (HP, Energy, MR, Follow%, AC, DR, Charm, BSDefense, NumAttacks, GameLimit)
  - +0x4B–0x50: Spell/ability data (spell ID at +0x4F confirmed)
  - +0x51–0x5F: Item drops (5 IDs + 5 percentages) — CONFIRMED
  - +0x60–0x68: Cash drops (Runic/Plat/Gold/Silver/Copper) — CONFIRMED
  - +0x6A–0x7C: **Ability Array** — 5 type/param pairs, types from WCC AbilityA system — CONFIRMED
    - Type 146 = MonsGuards (guard mechanic), 28 = Magical, 139 = SpellImmu, etc.
  - +0x7E: Weapon Number (Items.md reference) — CONFIRMED
  - +0x80–0x88: Between-rounds spell IDs (5 slots, Spells.md refs) — CONFIRMED
  - +0x92: Experience (u32) — CONFIRMED
  - +0x96–0x9D: Between-rounds cast percentages (cumulative u8) + levels — CONFIRMED
  - +0x9E: Group / spawn lair ID (WCC offset 84) — NOT min damage
  - +0xA0–0xBC: Attack arrays (min/max damage + energy, 5 slots) — CONFIRMED
  - +0xC0: Create Spell — CONFIRMED (hydra=90)
  - +0xC7: Undead flag — CONFIRMED
  - +0xC8: Attack Hit Spell — CONFIRMED
- Paths.md uses a non-standard record format (no 0x80 marker for full records)
  and stores routing metadata as an index to .mp files on disk
- Spell extended data (+0x64–0x9B) contains bitmask arrays (possibly class/level
  restrictions) with repeating 0x2020/0x2000 patterns — needs further analysis
- Spell extended data (+0x64–0x9B) contains ability effect arrays not yet mapped
