"""
MegaMUD v2.0 Alpha - Game State Struct Offsets
Reverse engineered from megamud.exe (PE32 i386, built Feb 24 2026)

The main game state struct is stored as window user data via
GetWindowLongA(hWnd, GWL_USERDATA) -- specifically index 4.
The WndProc is at 0x00447050, STAT dump function at 0x0048a7c0,
INI save at 0x004322a0, INI load at 0x004387e0.

All offsets are relative to the struct base pointer.
"""

# --- HWND / Window ---
HWND_OFFSET = 0x0C           # HWND of main window

# --- UI Settings ---
SOUND_ENABLED = 0x14C
SHOW_TALK = 0x150             # Not used directly but stored
SHOW_WHO = 0x154
SHOW_STAT = 0x158             # these seem off, check
SHOW_SESS = 0x15C
SHOW_TIME = 0x160
SHOW_RATE = 0x164
SHOW_PARTY = 0x168

# --- Last Actions (debug/trace) ---
LAST_ACTION_IDS = 0x1A8       # int32[10] - action type IDs
LAST_ACTION_STRS = 0x1D0      # char[50][10] - action description strings (0x32 bytes each)

# --- Room / Path Data ---
ROOM_DB_COUNT = 0x2C18        # int32 - number of rooms in database
ROOM_DB_PTR = 0x2C20          # ptr -> room entry array (each entry has checksum at +0x44, name at +0x06)
ROOM_NAME = 0x2D9C            # char[] - current room name
ROOM_CHECKSUM = 0x2E70        # uint32 - current room checksum/hash
ROOM_EXITS = 0x2E78           # int32[10] - exit types per direction (0=none, 2=closed, 3=open, 4=special, 5=unlocked)

# --- Monster/Entity List ---
ENTITY_LIST_COUNT = 0x1ED4    # int32 - entities in room
ENTITY_LIST_PTR = 0x1EE0      # ptr -> entity array (type at +0x00: 1=player, 2=monster, 3=item; name at +0x10; qty at +0x0C; known at +0x34/+0x38)

# --- Path/Navigation ---
PATHING_ACTIVE = 0x5664       # int32 - non-zero if currently pathing
LOOPING = 0x5668              # int32 - looping flag
AUTO_ROAMING = 0x566C         # int32 - auto-roaming flag
IS_LOST = 0x5670              # int32 - player is lost
IS_FLEEING = 0x5674           # int32 - player is fleeing
IS_RESTING = 0x5678           # int32 - player is resting
IS_MEDITATING = 0x567C        # int32 - player is meditating
IS_SNEAKING = 0x5688          # int32 - player is sneaking
IS_HIDING = 0x5690            # int32 - player is hiding
IN_COMBAT = 0x5698            # int32 - player is in combat
BACKTRACK_ROOMS = 0x358C      # int32 - rooms to backtrack
RUN_AWAY_ROOMS = 0x5508       # int32 - rooms to run away
ROOM_DARKNESS = 0x5528        # int32 - 0=normal, 2=dark, 3=pitch black

# --- Tick/Timer ---
TICK_COUNT_A = 0x9468         # tick/timer value A
TICK_COUNT_B = 0x9508         # tick/timer value B
TIMER_WAIT = 0x35B0           # used in party wait calculation
LAG_WAIT_SECONDS = 0x8A98     # int32 - current lag wait in seconds

# --- Health Thresholds (sequential i32s at 0x3788) ---
HP_FULL_PCT = 0x3788          # HpFull% - HP considered "full"
HP_REST_PCT = 0x378C          # HpRest% - rest when HP below this
HP_HEAL_PCT = 0x3790          # HpHeal% - cast heal when below this
HP_HEAL_ATT_PCT = 0x3794      # HpHealAtt% - heal in combat when below this
HP_RUN_PCT = 0x3798           # HpRun% - run away when below this (0=disabled)
HP_LOGOFF_PCT = 0x379C        # HpLogoff% - emergency logoff threshold
HP_HEAL_PERIOD = 0x37A0       # HpHealPeriod - heal check interval (seconds)
MANA_FULL_PCT = 0x37A4        # ManaFull% - mana considered "full"
MANA_REST_PCT = 0x37A8        # ManaRest% - rest when mana below this
MANA_HEAL_PCT = 0x37AC        # ManaHeal% - min mana to cast heal
MANA_HEAL_ATT_PCT = 0x37B0    # ManaHealAtt% - min mana to heal in combat
MANA_RUN_PCT = 0x37B4         # ManaRun% - run when mana below this
MANA_BLESS_PCT = 0x37B8       # ManaBless% - min mana to cast bless
MANA_MULT_ATT_PCT = 0x37BC    # ManaMultAtt% - min mana for multi-attack
MANA_PRE_ATT_PCT = 0x37C0     # ManaPreAtt% - min mana for pre-attack
MANA_ATTACK_PCT = 0x37C4      # ManaAttack% - min mana for attack spell
USE_MEDITATE = 0x37C8         # UseMeditate - meditate instead of rest
MEDITATE_B4_REST = 0x4E88     # MeditateB4Rest - meditate before resting
PRE_REST_CMD = 0x5045         # char[] - command to send before resting
POST_REST_CMD = 0x50AA        # char[] - command to send after resting

# --- Player Identity ---
PLAYER_NAME = 0x537C          # char[] - player name (null-terminated)
PLAYER_SURNAME = 0x5387       # char[] - player surname
PLAYER_GANG = 0x539A          # char[] - gang name
PLAYER_CPS = 0x53C0           # int32 - character points

# --- Player Exp (64-bit) ---
PLAYER_EXP_LO = 0x53B0       # int32 - experience (low dword)
PLAYER_EXP_HI = 0x53B4       # int32 - experience (high dword)
PLAYER_NEED_LO = 0x53B8      # int32 - exp needed to level (low dword)
PLAYER_NEED_HI = 0x53BC      # int32 - exp needed to level (high dword)

# --- Player Core ---
PLAYER_LIVES = 0x53C0         # int32
PLAYER_RACE = 0x53C4          # int32 (race ID, not string)
PLAYER_CLASS = 0x53C8         # int32 (class ID, not string)
PLAYER_LEVEL = 0x53D0         # int32

# --- Player HP / Mana ---
PLAYER_CUR_HP = 0x53D4        # int32
PLAYER_MAX_HP = 0x53DC        # int32
PLAYER_CUR_MANA = 0x53E0      # int32
PLAYER_MAX_MANA = 0x53E8      # int32
PLAYER_ENCUMB_LO = 0x53EC     # int32 - encumbrance (current/max pair written as "%d/%d")
PLAYER_ENCUMB_HI = 0x53F0     # int32

# --- Player Stats ---
PLAYER_STRENGTH = 0x53F4      # int32
PLAYER_AGILITY = 0x53F8       # int32
PLAYER_INTELLECT = 0x53FC     # int32
PLAYER_WISDOM = 0x5400        # int32
PLAYER_HEALTH = 0x5404        # int32 (constitution/health stat)
PLAYER_CHARM = 0x5408         # int32
PLAYER_PERCEPTION = 0x540C    # int32
PLAYER_STEALTH = 0x5410       # int32
PLAYER_THIEVERY = 0x5414      # int32
PLAYER_TRAPS = 0x5418         # int32
PLAYER_PICKLOCKS = 0x541C     # int32
PLAYER_TRACKING = 0x5420      # int32
PLAYER_MARTIAL_ARTS = 0x5424  # int32
PLAYER_MAGIC_RES = 0x5428     # int32
PLAYER_SPELL_CASTING = 0x542C # int32

# --- Player Flags ---
ON_ENTRY_ACTION = 0x54B4      # int32 - 0=do nothing, 1=resume loop, 2=auto-roam, 3=?
MSG_CODE = 0x54D4             # int32 - last WM_ message code received

# --- Connection ---
CONNECTED = 0x563C            # int32 - non-zero if connected to BBS
IN_GAME = 0x5644              # int32 - non-zero if in the game world
FIX_STEPS = 0x565C            # int32

# --- Status Effects ---
IS_BLINDED = 0x56AC           # int32
IS_CONFUSED = 0x56B0          # int32
IS_POISONED = 0x56B4          # int32 - flag
POISON_AMOUNT = 0x56B8        # int32 - poison damage
IS_DISEASED = 0x56BC          # int32
IS_LOSING_HP = 0x56C0         # int32 - HP drain
IS_REGENERATING = 0x56C4      # int32
IS_FLUXING = 0x56C8           # int32
IS_HELD = 0x56CC              # int32
IS_STUNNED = 0x56D0           # int32
RESTING_TO_FULL_HP = 0x56E0   # int32
RESTING_TO_FULL_MANA = 0x56E4 # int32

# --- Party Runtime ---
PARTY_SIZE = 0x35F0           # int32 - number of party members
PARTY_LEADER_NAME = 0x3748    # char[] - party leader name
PARTY_MEMBERS = 0x35F8        # struct[N] - party member array, each 0x38 (56) bytes:
                               #   +0x00 = char[] name
                               #   +0x0C = int32 HP percentage?
                               #   +0x10 = int32 current HP
                               #   +0x14 = int32 max HP
                               #   +0x28 = int32 flags (bit 7=hungup, bit 3=missing, bit 2=invited)
FOLLOWING = 0x5788             # int32 - following flag
LOST_FROM_LEADER = 0x578C     # int32
WAITING_ON_MEMBER = 0x5790    # int32
HOLDING_UP_LEADER = 0x5794    # int32

# --- Party Settings (Settings → Party tab, all WRITABLE) ---
PARTY_RANK = 0x3548           # int32 - Party Rank: 1=front, 2=mid, 3=back
ATTACK_LAST = 0x354C          # int32 - "Attack last in party"
SHARE_DAMAGE = 0x3550         # int32 - "Share damage"
ASK_HEALTH = 0x3554           # int32 - "Request party health"
DEFEND_PARTY = 0x355C         # int32 - "Defend party when PvP"
ATT_LEADER_MSTR = 0x3560      # int32 - "Attack what other members attack"
SHARE_CASH = 0x3564           # int32 - "Auto-share collected cash"
HELP_BASH = 0x3574            # int32 - "Help leader bash doors"
IGNORE_PARTY = 0x3578         # int32 - "Ignore @party when following"
IGNORE_WAIT = 0x357C          # int32 - "Ignore @wait when leading"
IGNORE_PANIC = 0x3580         # int32 - "Ignore @panic when leading"
SEND_PANIC = 0x3584           # int32 - "Say @panic when leading"
ATTACK_REVERSE = 0x1EE4       # int32 - "Attack in reverse order"
PAR_PERIOD = 0x3590           # int32 - "'pa' frequency (seconds)" - party status poll
PARTY_MAX_MSTRS = 0x3594      # int32 - "Max. monsters when partying"
PARTY_MAX_EXP = 0x3598        # int32 - "Max. monster experience"
PARTY_HEAL1_PCT = 0x35A0      # int32 - "Party Heal — Minor at _%"
PARTY_HEAL2_PCT = 0x35A4      # int32 - "Party Heal — Major at _%"
PARTY_ASK_HEAL_PCT = 0x35A8   # int32 - "Request healing at _%"
PARTY_WAIT_PCT = 0x35AC       # int32 - "Wait if members are below _%"
PARTY_WAIT_MAX = 0x35B0       # int32 - "If leading, wait only (mins)"
PARTY_BLESS_WAIT1 = 0x35B4    # int32 - Bless 1 Timeout (secs) - re-cast timer
PARTY_BLESS_WAIT2 = 0x35B8    # int32 - Bless 2 Timeout (secs)
PARTY_BLESS_WAIT3 = 0x35BC    # int32 - Bless 3 Timeout (secs)
PARTY_BLESS_WAIT4 = 0x35C0    # int32 - Bless 4 Timeout (secs)
PARTY_HEAL1_CMD = 0x35C4      # char[21] - Party Heal 1 spell command
PARTY_HEAL2_CMD = 0x35D9      # char[21] - Party Heal 2 spell command
PARTY_BLESS1_CMD = 0x4FF1     # char[21] - Party Bless 1 spell
PARTY_BLESS2_CMD = 0x5006     # char[21] - Party Bless 2 spell
PARTY_BLESS3_CMD = 0x501B     # char[21] - Party Bless 3 spell
PARTY_BLESS4_CMD = 0x5030     # char[21] - Party Bless 4 spell

# --- Automation Toggles (all WRITABLE, 1=on, 0=off) ---
AUTO_COMBAT = 0x4D00          # int32 - auto-combat. Walk sets 1, Run sets 0.
AUTO_NUKE = 0x4D04            # int32 - auto-nuke (offensive spells)
AUTO_HEAL = 0x4D08            # int32 - auto-heal self
AUTO_BLESS = 0x4D0C           # int32 - auto-bless (buff spells)
AUTO_LIGHT = 0x4D10           # int32 - auto-light in dark rooms
AUTO_CASH = 0x4D14            # int32 - auto-pick up cash
AUTO_GET = 0x4D18             # int32 - auto-pick up items
AUTO_SEARCH = 0x4D1C          # int32 - auto-search rooms
AUTO_SNEAK = 0x4D20           # int32 - auto-sneak movement
AUTO_HIDE = 0x4D24            # int32 - auto-hide after combat
AUTO_TRACK = 0x4D28           # int32 - auto-track monsters

# --- Default Profiles (saved presets, same 11-switch order) ---
DEF_PROFILE_1 = 0x4D2C        # int32[11] - default profile 1
DEF_PROFILE_2 = 0x4D58        # int32[11] - default profile 2
START_TASK = 0x4D84            # int32 - on-login task (0=nothing)

# --- Spell Commands (Settings → Spells tab, all char[21] strings) ---
HEAL_CMD = 0x4E40             # HealCmd - heal spell command
REGEN_CMD = 0x4E55            # RegenCmd - regen spell command
FLUX_CMD = 0x4E6A             # FluxCmd - flux spell command
FLUX_MIN = 0x4E80             # int32 - FluxMin - minimum mana for flux
BLIND_CMD = 0x4E8C            # BlindCmd - cure blindness command
POISON_CMD = 0x4EA1           # PoisonCmd - cure poison command
DISEASE_CMD = 0x4EB6          # DiseaseCmd - cure disease command
FREEDOM_CMD = 0x4ECB          # FreedomCmd - cure held/freedom command
LIGHT_CMD = 0x4EE0            # LightCmd - light spell command
HP_FULL_CMD = 0x4EF5          # HpFullCmd - full HP heal command
MA_FULL_CMD = 0x4F0A          # MaFullCmd - full mana restore command
BLESS_CMD1 = 0x4F1F           # BlessCmd1 - bless spell 1
BLESS_CMD2 = 0x4F34           # BlessCmd2 - bless spell 2
BLESS_CMD3 = 0x4F49           # BlessCmd3 - bless spell 3
BLESS_CMD4 = 0x4F5E           # BlessCmd4 - bless spell 4
BLESS_CMD5 = 0x4F73           # BlessCmd5 - bless spell 5
BLESS_CMD6 = 0x4F88           # BlessCmd6 - bless spell 6
BLESS_CMD7 = 0x4F9D           # BlessCmd7 - bless spell 7
BLESS_CMD8 = 0x4FB2           # BlessCmd8 - bless spell 8
BLESS_CMD9 = 0x4FC7           # BlessCmd9 - bless spell 9
BLESS_CMD10 = 0x4FDC          # BlessCmd10 - bless spell 10

# --- Combat Settings (from binary disassembly) ---
CAN_BACKSTAB = 0x37D0         # CanBackStab
DONT_BS_IF_MULTI = 0x37D4     # DontBsIfMulti
RUN_IF_BS_FAILS = 0x37D8      # RunIfBsFails
ATTACK_NEUTRAL = 0x37DC       # AttackNeutral
POLITE_ATTACKS = 0x37E0       # PoliteAttacks
MAX_MSTRS = 0x4D88            # MaxMstrs - max monsters to engage
MAX_MSTR_EXP = 0x4D90         # MaxMstrExp - max monster exp to engage
MAX_CAST_CNT = 0x4DAC         # MaxCastCnt
ATT_MAX_DMG = 0x4DB0          # AttMaxDmg
PRE_CAST_CNT = 0x4DB4         # PreCastCnt
PRE_MAX_DMG = 0x4DB8          # PreMaxDmg
MULT_CAST_CNT = 0x4DBC        # MultCastCnt
MULT_MSTR_CNT = 0x4DC0        # MultMstrCnt - min enemies for multi-attack
MULT_MAX_DMG = 0x4DC4         # MultMaxDmg
BASH_MAX = 0x54E4             # BashMax - max bash attempts per door
PICK_MAX = 0x54EC             # PickMax - max pick-lock attempts
DISARM_MAX = 0x54F4           # DisarmMax - max disarm trap attempts
SEARCH_MAX = 0x54FC           # SearchMax - max searches per room
RUN_ROOMS = 0x550C            # RunRooms - rooms to flee when running
AUTOCOMBAT_SLOT = 0x574C      # int32 - -1 when certain auto functions triggered

# --- Combat String Settings ---
ATTACK_CMD = 0x51D9           # char[] - AttackCmd - attack command (e.g. "a")
BS_WEAPON = 0x52E5            # char[] - BsWeapon - backstab weapon name
NRM_WEAPON = 0x52C6           # char[] - NrmWeapon - normal weapon name
ALT_WEAPON = 0x5304           # char[] - AltWeapon - alternate weapon name
SHIELD_ITEM = 0x5323          # char[] - Shield - combat shield item name
USE_SHIELD_FOR_BS = 0x5354    # int32 - UseShieldForBS - equip shield for backstab
USE_NRM_WEAP_FOR_SPELLS = 0x5358  # int32 - UseNrmWeapForSpells
MULT_ATTACK_CMD = 0x51E4      # char[] - MultAttack - multi-attack spell command
PRE_ATTACK_CMD = 0x51EF       # char[] - PreAttack - pre-attack spell command
ATTACK_SPL_CMD = 0x51FA       # char[] - AttackSpl - attack spell command

# --- Behavior / Other Settings (from binary disassembly) ---
DISABLE_EVENTS = 0x1E18       # DisableEvents
EVENTS_AFK_ONLY = 0x1E1C      # EventsAfkOnly
CAN_PICK_LOCKS = 0x37C8       # CanPickLocks
CAN_DISARM_TRAPS = 0x37CC     # CanDisarmTraps
AUTO_TRAIN = 0x3780           # AutoTrain
RELOG_INSTEAD = 0x3810        # RelogInstead
HANGUP_NOT_AFK = 0x3814       # HangupNotAfk
HANGUP_ALL_OFF = 0x3818       # HangupAllOff
HANGUP_NAKED = 0x381C         # HangupNaked
IGNORE_POISON = 0x3A34        # IgnorePoison
IGNORE_BLIND = 0x3A38         # IgnoreBlind
IGNORE_CONFUSION = 0x3A3C     # IgnoreConfusion
BLESS_RESTING = 0x3A40        # BlessResting
BLESS_COMBAT = 0x3A44         # BlessCombat
SEARCH_NEED_ITEM = 0x3A48     # SearchNeedItem
RUN_BACKWARDS = 0x3A4C        # RunBackwards
BREAK_B4_RUNNING = 0x3A50     # BreakB4Running
LIGHT_DIM_ROOMS = 0x5810      # LightDimRooms
MUST_SNEAK = 0x5814           # MustSneak
SUPER_STEALTH = 0x5818        # SuperStealth - supernatural stealth
AUTO_AUTO_COMBAT = 0x581C     # AutoAutoCombat - re-enable combat on arrival
AUTO_AUTO_HEAL = 0x5820       # AutoAutoHeal - re-enable heal on arrival
AUTO_CONNECT = 0x8AA0         # AutoConnect - auto-reconnect
NO_MODE_DEFS = 0x4D84         # NoModeDefs

# --- Talk / AFK ---
AUTO_AFK = 0x3820             # AutoAfk
AUTO_AFK_OFF = 0x3824         # AutoAfkOff
AFK_MINIMIZED = 0x3828        # AfkMinimized - go AFK when minimized
AFK_TIMEOUT = 0x382C          # AfkTimeout (minutes)
AFK_REPLY = 0x3830            # char[] - AfkReply - AFK auto-reply message
CMD_REPLY = 0x3931            # char[] - CmdReply - invalid command reply
LOG_TALK = 0x3A54             # int32 - LogTalk - enable talk logging
GREET_PLAYERS = 0x3A58        # GreetPlayers
LOOK_PLAYERS = 0x3A5C         # LookPlayers
NO_REMOTE_CMDS = 0x3A60       # NoRemoteCmds
NO_GANG_CMDS = 0x3A60         # NoGangCmds (same offset? verify)
WARN_REMOTE = 0x3A68          # WarnRemote
DIVERT_LOCAL = 0x3A6C         # int32 - DivertLocal - divert local chat to talk window
DIVERT_TELE = 0x3A70          # int32 - DivertTele - divert telepathy
DIVERT_GOSSIP = 0x3A74        # int32 - DivertGossip
DIVERT_GANG = 0x3A78          # int32 - DivertGang
DIVERT_BROAD = 0x3A7C         # int32 - DivertBroad - divert broadcast
LOG_FILE = 0x1707             # char[] - LogFile - talk log file path

# --- Display ---
SHOW_INFO = 0x3800            # ShowInfo
SHOW_ROUNDS = 0x3804          # ShowRounds
SHOW_SEND = 0x3808            # ShowSend
SHOW_RUN_MSG = 0x380C         # ShowRunMsg
POPUP_AFK_MSGS = 0x017C       # int32 - PopupAfkMsgs
SHOW_ALL_AFK = 0x0180         # int32 - ShowAllAfk
AUTO_HIDE_PARTY = 0x0184      # int32 - AutoHideParty
SCALE_ANSI = 0x1A44           # int32 - ScaleAnsi
ANSI_ROWS = 0x1A48            # int32 - AnsiRows
ANSI_COLS = 0x1A4C            # int32 - AnsiCols

# --- PvP ---
PVP_ACTION = 0x3A80           # PvpAction
REDIAL_PVP = 0x3A84           # RedialPvp - redial after PvP death
PVP_SAFE_PERIOD = 0x8A88      # int32 - PvpSafePeriod - safe period (seconds)
FLEE_ROOMS_PVP = 0x3A8C       # int32 - FleeRooms - rooms to flee in PvP
FLEE_TIMEOUT = 0x3A90          # int32 - FleeTimeout - flee timeout (seconds)
TRACK_ENEMIES = 0x3A94        # TrackEnemies
TRACK_DELAY = 0x3A9C          # int32 - TrackDelay - delay between tracking (seconds)
HIDE_DELAY = 0x3AA0           # int32 - HideDelay - delay before hiding after PvP
NOTIFY_GANG = 0x3AA4          # int32 - NotifyGang - notify gang on PvP
PVP_FLEE_ROOM = 0x3AA8        # char[] - PvpFleeRoom - room to flee to
PVP_SPELL1 = 0x5205           # char[] - PvP attack spell 1
PVP_SPELL2 = 0x5256           # char[] - PvP attack spell 2

# --- Comms / Connection ---
AUTO_ZMODEM = 0x9444          # AutoZModem
REDIAL_MAX = 0x8A5C           # int32 - RedialMax - max redial attempts
REDIAL_PAUSE = 0x8A54         # int32 - RedialPause - pause between redials (seconds)
REDIAL_CONNECT = 0x8A60       # int32 - RedialConnect - redial on disconnect
REDIAL_CARRIER = 0x8A64       # int32 - RedialCarrier - redial on carrier loss
REDIAL_NO_RESPONSE = 0x8A68   # int32 - RedialNoResponse - redial on no response
REDIAL_CLEANUP = 0x8A6C       # int32 - RedialCleanup - cleanup on redial
LOGOFF_LOW_EXP = 0x8A70       # int32 - LogoffLowExp - logoff if exp rate low
CLEANUP_PERIOD = 0x8A84       # int32 - CleanupPeriod (minutes)
MIN_EXP_RATE = 0x8A94         # int32 - MinExpRate - minimum exp rate threshold
LAG_WAIT = 0x8A98             # int32 - LagWait (same as LAG_WAIT_SECONDS)

# --- Key runtime flags ---
GO_FLAG = 0x564C              # int32 - WRITABLE master go/stop toggle. 1=go, 0=stop.
MODE = 0x54BC                 # int32 - 11=idle, 14=walking/running, 17=lost
STEPS_REMAINING = 0x54D8      # int32 - countdown of steps in current path segment
AUTO_COMBAT_TOGGLE = AUTO_COMBAT  # alias for backwards compat

# --- Goto / Path / Loop ---
DEST_CHECKSUM = 0x2EA4        # u32 - WRITABLE destination room checksum for goto
UNK_54DC = 0x54DC             # i32 - set to -1 before loop, cleared to 0 on start
PATH_FILE_NAME = 0x5834       # char[76] - WRITABLE current .mp filename
PATH_WP_0 = 0x5880            # u32 - WRITABLE waypoint checksum 0 (from_checksum)
PATH_WP_1 = 0x5884            # u32 - WRITABLE waypoint checksum 1 (to_checksum)
PATH_TOTAL_STEPS = 0x5894     # i32 - WRITABLE total steps in path
CUR_PATH_STEP = 0x5898        # i32 - WRITABLE current step index (1-based)

# --- Session / Combat Statistics ---
# WARNING: Session stats (exp meter, accuracy, etc.) are stored in a HEAP
# ALLOCATION managed by the Player Statistics dialog, NOT in the main struct.
# The 0x95xx-0x96xx range contains pointers/stack data, not counters.
#
# To RESET stats: use SendMessage(BM_CLICK) on Player Statistics buttons:
#   Control ID 1721 = Experience reset (duration, exp made, exp rate)
#   Control ID 1722 = Accuracy reset (hit%, crit%, BS, rounds)
#   Control ID 1723 = Other reset (sneak%, dodge%, collected)
# See scripts/reset_stats.py
#
# To READ stats: use GetWindowTextA on static control IDs:
#   1134=duration, 1141=exp_made, 1142=exp_needed, 1143=exp_rate,
#   1209=will_level_in, 1174=hit%, 1106=crit%, 1064=BS%, etc.
#
# Verified internal reset handler offsets (VA 0x470720, struct-relative):
STAT_TICK_SNAPSHOT_LO = 0x95D8  # reset copies TICK_COUNT_A (0x9468) here
STAT_TICK_SNAPSHOT_HI = 0x95DC  # reset copies TICK_COUNT_B (0x946C) here
STAT_TICK_SNAPSHOT2_LO = 0x95E0 # second tick snapshot
STAT_TICK_SNAPSHOT2_HI = 0x95E4 # second tick snapshot
STAT_HP_SNAPSHOT = 0x9618       # reset copies PLAYER_CUR_HP (0x53D4) here
STAT_MANA_SNAPSHOT = 0x961C     # reset copies PLAYER_CUR_MANA (0x53E0) here

# --- Cash / Item Pickup (gated by AUTO_CASH/AUTO_GET) ---
WANT_COPPER = 0x3208          # int32 - WantCopper
WANT_SILVER = 0x320C          # int32 - WantSilver
WANT_GOLD = 0x3210            # int32 - WantGold
WANT_PLAT = 0x3214            # int32 - WantPlat
WANT_RUNIC = 0x3218           # int32 - WantRunic
STASH_COIN = 0x321C           # int32 - StashCoin - auto-stash coins
GET_AFTER_COMBAT = 0x3224     # int32 - GetAfterCombat
DROP_COINS = 0x3228           # int32 - DropCoins - drop unwanted coin types
LIMIT_WEALTH = 0x322C         # int32 - LimitWealth
MIN_WEALTH = 0x3230           # int32 - MinWealth
MAX_WEALTH = 0x3234           # int32 - MaxWealth
LIMIT_COINS = 0x3238          # int32 - LimitCoins
MAX_COINS = 0x323C            # int32 - MaxCoins
BANK_NAME = 0x3240            # char[] - Bank name for deposits
STASH_COIN_TYPE = 0x326C      # int32 - StashCoinType - coin type to stash
DONT_BE_HEAVY = 0x3220        # int32 - DontBeHeavy
DONT_BE_MEDIUM = 0x3224       # int32 - DontBeMedium

# --- Alerts (each char[257] - sound file path) ---
ALERT_IDLE = 0x3AD8           # AlertIdle
ALERT_TRAIN = 0x3BD9          # AlertTrain
ALERT_LOW_HPS = 0x3CDA        # AlertLowHPs
ALERT_HANGUP = 0x3DDB         # AlertHangup
ALERT_PVP = 0x3EDC            # AlertPvP
ALERT_TELE = 0x3FDD           # AlertTele
ALERT_PAGE = 0x40DE           # AlertPage
ALERT_TALK = 0x41DF           # AlertTalk
ALERT_GANGPATH = 0x42E0       # AlertGangpath
ALERT_GOSSIP = 0x43E1         # AlertGossip
ALERT_AUCTION = 0x44E2        # AlertAuction
ALERT_BROADCAST = 0x45E3      # AlertBroadcast
ALERT_WHEN_AFK = 0x46E4       # int32 - AlertWhenAfk - only alert when AFK

# --- Auto-roam ---
AUTO_MOVE = 0x3274            # uint16 - AutoMove - direction bitmask (255=all)
AUTO_DOOR = 0x3276            # uint16 - AutoDoor - auto-open doors
AUTO_ROOM_BASE = 0x3278       # char[41][10] - AutoRoom0-9, stride 0x29
AUTO_CMD_BASE = 0x3412        # char[31][10] - AutoCmd0-9, stride 0x1F

# --- Other / Misc ---
NO_HANGUP = 0x5A08            # int32
GANGPATH_FLAG = 0x3A64        # int32 - gang path related
ROOM_MATCH_PTR = 0x3154       # ptr - something related to room matching / memory alloc
ROOM_MATCH_2 = 0x314C         # ptr
SCROLL_MEM = 0x37CC           # int32 - ScrollMem - scroll memory buffer size
DEL_PLAYERS = 0x5520          # int32 - DelPlayers - days before deleting inactive
CMD_SPLIT_CHAR = 0x5615       # char[] - CmdSplitChar - command separator (";")
ENTRY_CMD = 0x510F            # char[] - EntryCmd - command on game entry
EXIT_CMD = 0x5174             # char[] - ExitCmd - command on game exit
PARTY_WAIT_CMD = 0x3753       # char[] - PartyWaitCmd - custom wait command
PARTY_RESUME_CMD = 0x3768     # char[] - PartyResumeCmd - custom resume command

# --- Security cookie (for stack canaries) ---
SECURITY_COOKIE = 0x51F080    # global, not struct-relative. Value: 0xBB40E64E (static at binary level)
