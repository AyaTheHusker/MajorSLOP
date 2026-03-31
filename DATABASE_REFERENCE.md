# MajorMUD Database Ecosystem Reference

How the three database layers relate: WCC game dats (Btrieve) -> MME export (Access) -> MegaMUD (MDB2).

## The Three Layers

### 1. WCC Dats (Btrieve) — Ground Truth
Location: `DAT Files V1.11p/wcc*.dat`
Format: Btrieve/Pervasive.SQL binary database
Contains: **ALL** game data, every field the game engine uses

| File | Records | Btrieve Record Size | Contents |
|------|---------|-------------------|----------|
| wccitem2.dat | ~1950 | 1072 bytes | Items (full descriptions, all abilities, all flags) |
| wccmp002.dat | ~1100 | 756 bytes | Monsters (all attacks, drops, descriptions, scripts) |
| wccspel2.dat | ~1400 | 260 bytes | Spells (all scaling, abilities, targeting) |
| wccrace2.dat | 13 | 126 bytes | Races (stat ranges, abilities) |
| wccclas2.dat | 15 | 156 bytes | Classes (HP, magery, weapon/armor types) |
| wccshop2.dat | 178 | 504 bytes | Shops (20 item slots, restocking) |
| wcctext2.dat | varies | 2024 bytes | Textblocks (scripting/actions) |
| wccuser2.dat | varies | 2028 bytes | Users/players (full character state) |
| wccacts2.dat | varies | 1010 bytes | Actions (emotes with 11 message forms) |
| wccmsg2.dat | varies | 256 bytes | Messages (3 lines of 74 chars) |
| wccbank2.dat | varies | 76 bytes | Bank accounts |
| wccknms2.dat | varies | ? | Known names |
| wccgang2.dat | varies | ? | Gangs |
| wccitow2.dat | varies | ? | Items on world (dropped items) |

### 2. MME Export (Access .mdb) — Filtered Subset
Location: `MMUD Explorer/data-v1.11p.mdb`
Format: Microsoft Access / JET database
Created by: Nightmare Redux's frmMME_Export

**What MME adds that WCC doesn't have:**
- "In Game" boolean (computed by cross-referencing rooms/lairs/textblocks)
- "Obtained From" text (traces where items come from)
- "Summoned By" text (traces monster spawn sources)
- "Learned From" / "Casted By" text (spell sources)
- Pre-calculated lair stats (avg exp, damage, HP, mob counts)
- "Script Value" per monster (theoretical exp/hr from 2000-round sims)

**What MME drops from WCC:**
- Item descriptions (6 lines of 60 chars each)
- Monster descriptions (4 lines of 70 chars each)
- Spell descriptions (2 lines of 50 chars each)
- Textblock content (full scripting data)
- Message text content
- All "unknown" fields
- Container cash contents
- Some internal flags

### 3. MegaMUD (MDB2) — Compact Working Copy
Location: `~/.wine/drive_c/MegaMUD/Default/*.md`
Format: MDB2 (C-Index/II proprietary B-tree)
Created by: MegaMUD importing from WCC dats + user edits

**MegaMUD stores MUCH less per record:**
- Items: 198-byte payload (vs 1072 WCC)
- Spells: 155-byte payload (vs 260 WCC)
- Monsters: ~200-300 bytes (vs 756 WCC) — TBD exact
- No descriptions at all
- No textblock references
- Simplified abilities (fewer slots)
- Shop name string instead of shop number reference

## "In Game" Detection (from Nightmare Redux source)

Items, monsters, and spells in the WCC dats include things NOT actually obtainable
in the game. The MME export uses a multi-pass cross-reference to determine what's real:

### Pass 1: Scan Rooms
- Placed items in rooms -> mark item In Game
- Hidden sign items (type 3) -> mark item In Game
- Shop assigned to room -> mark shop In Game
- Permanent NPC -> mark monster In Game
- Room spell -> mark spell In Game
- Exit requirements (ticket/key/cast) -> mark item/spell In Game
- Room lairs (MinIndex..MaxIndex + MonsterType) -> build MonGroup index

### Pass 2: Scan Shops
- All items stocked in In Game shops -> mark item In Game

### Pass 3-20: Iterative Cross-Reference (repeats until no new marks)
**Scan Monsters:**
- Monster whose Group+Index appears in any room lair -> In Game
- Monster name starts with "sdf" -> NEVER In Game (test/placeholder)
- In Game monster drops -> mark items In Game
- Monster abilities: summon(12), learnspell(42), castssp(43), endcast(151), killspell(153) -> mark targets
- Monster abilities: textblock(148), deathtext(155) -> mark textblocks
- Attack hit spells, mid-round spells, death spell, create spell -> mark spells
- Greet/talk textblocks -> mark for scanning

**Scan Spells:**
- Spell abilities can chain-reference more items, monsters, spells, textblocks

**Scan Items:**
- Item abilities: same chain rules as monsters (summon, learn, cast, textblock)
- Read textblock -> mark for scanning

**Scan Textblocks:**
- Textblock scripts can reference items, monsters, spells, other textblocks
- Commands like "give item", "create monster", "cast spell" -> mark targets

**Termination:** Loop ends when a full pass marks nothing new, or after 20 passes max.

## Complete Field Maps (from Nightmare Redux modFieldmaps.bas)

### ItemRecType (1072 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field              MegaMUD Items.md?
0       4     Long              Number             Yes (as key + u16)
4       2     Integer           unknown1           No
6       2     Integer           GameLimit          Uncertain
8       2     Integer           unknown2           No
10      2     Integer           unknown3           No
12      2     Integer           unknown4           No
14      2     Integer           unknown5           No
16      157   String*156+Byte   EmptySpace         No
173     30    String*29+Byte    Name               Yes (30 bytes)
203     61    String*60+Byte    Desc1              NO
264     61    String*60+Byte    Desc2              NO
325     61    String*60+Byte    Desc3              NO
386     61    String*60+Byte    Desc4              NO
447     61    String*60+Byte    Desc5              NO
508     61    String*60+Byte    Desc6              NO
569     61    String*60+Byte    Desc7 (not desc)   NO
630     61    String*60+Byte    Desc8 (not desc)   NO
691     61    String*60+Byte    Desc9 (not desc)   NO
752     2     Integer           unknown6 (limit?)  Uncertain
754     2     Integer           Weight             Yes
756     2     Integer           Type               Yes
758     40    Integer[20]       AbilityA[0-19]     Partial
798     2     Integer           Uses               Yes
800     2     Integer           unknown7           No
802     2     Integer           Cost               Yes
804     20    Integer[10]       Class[0-9]         Yes (restrictions)
824     2     Integer           unknown8           No
826     2     Integer           unknown9           No
828     2     Integer           unknown10          No
830     2     Integer           MinHit             Yes
832     2     Integer           MaxHit             Yes
834     2     Integer           AC                 Yes
836     20    Integer[10]       Race[0-9]          No
856     20    Integer[10]       unknown11[0-9]     No
876     40    Integer[20]       Negate[0-19]       No
916     2     Integer           Weapon             Yes
918     2     Integer           Armour             Yes
920     2     Integer           WornOn             Yes
922     2     Integer           Accuracy           Yes
924     2     Integer           DR                 Yes
926     1     Byte              Gettable           Yes
927     1     Byte              unknown12          No
928     2     Integer           ReqStr             Yes
930     10    Integer[5]        unknown13a         No
940     4     Long              OpenRunic          No
944     4     Long              OpenPlatinum       No
948     4     Long              OpenGold           No
952     4     Long              OpenSilver         No
956     4     Long              OpenCopper         No
960     30    Integer[15]       unknown13b         No
980     2     Integer           Speed              Yes
982     2     Integer           unknown14          No
984     40    Integer[20]       AbilityB[0-19]     Partial
1024    2     Integer           unknown15          No
1026    4     Long              HitMsg             No
1030    4     Long              MissMsg            No
1034    4     Long              ReadTB             No
1038    4     Long              DistructMsg        No
1042    12    Integer[6]        unknown16          No
1054    1     Byte              NotDroppable       Yes
1055    1     Byte              CostType           Yes
1056    1     Byte              RetainAfterUses    Yes
1057    1     Byte              Robable            No
1058    1     Byte              DestroyOnDeath     Yes
1059    1     Byte              unknown19          No
```

### MonsterRecType (756 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       4     Long              Number
4       50    String*50         EmptySpace
54      30    String*29+Byte    Name
84      2     Integer           Group
86      2     Integer           nothingXX1
88      4     Long              ExpMulti
92      2     Integer           Index
94      2     Integer           nothingXX3
96      4     Long              Something2
100     4     Long              WeaponNumber
104     2     Integer           DR
106     2     Integer           AC
108     2     Integer           Something3
110     2     Integer           Follow
112     2     Integer           MR (Magic Resist)
114     2     Integer           BSDefence
116     4     Long              Experience
120     2     Integer           Hitpoints
122     2     Integer           Energy
124     2     Integer           HPRegen
126     20    Integer[10]       AbilityA[0-9]
146     20    Integer[10]       AbilityB[0-9]
166     2     Integer           GameLimit
168     2     Integer           Active
170     2     Integer           Type
172     1     Byte              nothing2
173     1     Byte              Undead
174     2     Integer           Alignment
176     2     Integer           nothing3
178     2     Integer           RegenTime
180     2     Integer           DateKilled
182     2     Integer           TimeKilled
184     4     Long              MoveMsg
188     4     Long              DeathMsg
192     40    Long[10]          ItemNumber[0-9]     (drop items)
232     20    Integer[10]       ItemUses[0-9]
252     10    Byte[10]          ItemDropPer[0-9]    (drop percentages)
262     2     Integer           nothing9
264     4     Long              Runic
268     4     Long              Platinum
272     4     Long              Gold
276     4     Long              Silver
280     4     Long              Copper
284     4     Long              GreetTxt
288     2     Integer           CharmLvL
290     2     Integer           Nothing16
292     4     Long              DescTxt
296     5     Byte[5]           AttackType[0-4]
301     1     Byte              Nothing22
302     10    Integer[5]        AttackAccuSpell[0-4]
312     5     Byte[5]           AttackPer[0-4]
317     1     Byte              Nothing17
318     10    Integer[5]        AttackMinHCastPer[0-4]
328     10    Integer[5]        AttackMaxHCastLvl[0-4]
338     2     Integer           Nothing18
340     20    Long[5]           AttackHitMsg[0-4]
360     20    Long[5]           AttackDodgeMsg[0-4]
380     20    Long[5]           AttackMissMsg[0-4]
400     10    Integer[5]        AttackEnergy[0-4]
410     2     Integer           Nothing19
412     4     Long              TalkTxt
416     2     Integer           CharmRes
418     2     Integer           Nothing21
420     10    Integer[5]        AttackHitSpell[0-4]
430     2     Integer           DeathSpellNumber
432     2     Integer           Nothing23
434     2     Integer           Nothing24
436     2     Integer           Nothing25
438     2     Integer           Nothing26
440     2     Integer           Nothing27
442     2     Integer           Nothing28
444     2     Integer           Nothing29
446     2     Integer           CreateSpellNumber
448     10    Integer[5]        SpellNumber[0-4]    (mid-round casts)
458     5     Byte[5]           SpellCastPer[0-4]
463     5     Byte[5]           SpellCastLvl[0-4]
468     71    String*70+Byte    DescLine1
539     71    String*70+Byte    DescLine2
610     71    String*70+Byte    DescLine3
681     71    String*70+Byte    DescLine4
752     1     Byte              Gender
753     1     Byte              Nothing14
754     2     Integer           Nothing15
```

### SpellRecType (260 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       2     Integer           Number
2       30    String*29+Byte    Name
32      51    String*50+Byte    DescA
83      51    String*50+Byte    DescB
134     2     Integer           N01
136     4     Long              CastMsgA
140     22    Integer[11]       N02
162     1     Byte              LevelCap
163     1     Byte              N03
164     1     Byte              MsgStyle
165     3     Byte[3]           N04
168     20    Integer[10]       AbilityB[0-9]
188     2     Integer           Energy
190     2     Integer           Level
192     2     Integer           Min
194     2     Integer           Max
196     2     Integer           SpellType
198     2     Integer           TypeOfResists
200     2     Integer           Difficulty
202     2     Integer           UNDEFINED01
204     2     Integer           Target
206     2     Integer           Duration
208     2     Integer           TypeOfAttack
210     2     Integer           UNDEFINED02
212     2     Integer           ResistAbility
214     2     Integer           MageryA
216     20    Integer[10]       AbilityA[0-9]
236     4     Long              CastMsgB
240     2     Integer           Mana
242     1     Byte              MaxIncrease
243     1     Byte              LVLSMaxIncr
244     2     Integer           MageryB
246     1     Byte              MinIncrease
247     1     Byte              LVLSMinIncr
248     1     Byte              DurIncrease
249     1     Byte              LVLSDurIncr
250     6     String*5+Byte     ShortName
256     4     Long              N06
```

### RaceRecType (126 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       2     Integer           Number
2       30    String*29+Byte    Name
32      2     Integer           MinInt
34      2     Integer           MinWil
36      2     Integer           MinStr
38      2     Integer           MinHea
40      2     Integer           MinAgl
42      2     Integer           MinChm
44      2     Integer           HPBonus (HP per level)
46      4     Long              nothing2
50      20    Integer[10]       AbilityA[0-9]
70      2     Integer           CP (starting CP)
72      20    Integer[10]       AbilityB[0-9]
92      4     Long              nothing3
96      2     Integer           nothing4
98      2     Integer           ExpChart
100     2     Integer           nothing5
102     2     Integer           MaxInt
104     2     Integer           MaxWil
106     2     Integer           MaxStr
108     2     Integer           MaxHea
110     2     Integer           MaxAgl
112     2     Integer           MaxChm
114     4     Long              Nothing6
118     4     Long              nothing7
122     4     Long              nothing8
```

### ClassRecType (156 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       2     Integer           Number
2       30    String*29+Byte    Name
32      2     Integer           MinHp
34      2     Integer           MaxHP
36      2     Integer           Exp (chart)
38      2     Integer           nothing1
40      2     Integer           nothing2
42      2     Integer           nothing3
44      20    Integer[10]       AbilityA[0-9]
64      2     Integer           MagicType
66      2     Integer           MagicLvL
68      2     Integer           Weapon (type)
70      2     Integer           Armour (type)
72      2     Integer           Combat (level)
74      20    Integer[10]       AbilityB[0-9]
94      2     Integer           nothing4
96      2     Integer           nothing5
98      2     Integer           Nothing6
100     4     Long              TitleText
```

### ShopRecType (504 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       4     Long              Number
4       40    String*39+Int     Name + AfterName
44      53    String*52+Byte    DescriptionA
97      53    String*52+Byte    DescriptionB
150     53    String*52+Byte    DescriptionC
203     2     Integer           ShopType
205     2     Integer           MinLvL
207     2     Integer           MaxLvl
209     2     Integer           MarkUp
211     2     Integer           nothing4
213     1     Byte              ClassLimit
214     1     Byte              nothingAA
215     80    Long[20]          ItemNumber[0-19]
295     40    Integer[20]       Max[0-19]
335     40    Integer[20]       Now[0-19] (current stock)
375     40    Integer[20]       RgnTime[0-19]
415     40    Integer[20]       RgnNumber[0-19]
455     20    Byte[20]          RgnPercentage[0-19]
```

### UserRecType (2028 bytes in WCC Btrieve)

```
Offset  Size  VB Type           Field
0       30    String*30         BBSName
30      11    String*10+Byte    FirstName
41      19    String*18+Byte    LastName
60      4     Long              NotExperience
64      20    Integer[10]       SpellCasted
84      20    Integer[10]       SpellValue
104     20    Integer[10]       SpellRoundsLeft
124     20    String*20         Title
144     2     Integer           Race
146     2     Integer           Class
148     2     Integer           Level
150     24    Integer[12]       Stat[0-11]
174     2     Integer           MaxHP
176     2     Integer           CurrentHP
178     2     Integer           MaxENC
180     2     Integer           CurrentENC
182     6     Integer[3]        Energy
188     4     Integer[2]        unknown1
192     2     Integer           MagicRes
194     2     Integer           MagicRes2
196     4     Long              MapNumber
200     4     Long              RoomNum
204     2     Integer           nothing2
206     4     Integer[2]        unknown2
210     2     Integer           nothing3
212     2     Byte[2]           unknown3
214     2     Integer           nothing4
216     400   Long[100]         Item[0-99]       (inventory, 100 slots!)
616     200   Integer[100]      ItemUses[0-99]
816     4     Long              nothing5
820     200   Long[50]          Key[0-49]
1020    100   Integer[50]       KeyUses[0-49]
1120    16    Long[4]           unknown4
1136    4     Long              BillionsOfExperience
1140    4     Long              MillionsOfExperience
1144    2     Integer           Nothing6
1146    200   Integer[100]      Spell[0-99]      (known spells)
1346    2     Integer           EvilPoints
1348    12    Long[3]           nothing7
1360    80    Long[20]          LastMap[0-19]
1440    80    Long[20]          LastRoom[0-19]
1520    2     Integer           nothing8
1522    2     Integer           BroadcastChan
1524    4     Long              unknown5
1528    2     Integer           Perception
1530    2     Integer           Stealth
1532    2     Integer           MartialArts
1534    2     Integer           Thievery
1536    2     Integer           MaxMana
1538    2     Integer           CurrentMana
1540    2     Integer           SpellCasting
1542    2     Integer           Traps
1544    2     Integer           unknown6
1546    2     Integer           PickLocks
1548    2     Integer           Tracking
1550    2     Integer           nothing9
1552    4     Long              Runic
1556    4     Long              Platinum
1560    4     Long              Gold
1564    4     Long              Silver
1568    4     Long              Copper
1572    4     Long              WeaponHand
1576    4     Long              nothing10
1580    80    Long[20]          WornItem[0-19]
1660    40    Integer[20]       unknown7
1700    2     Integer           unknown8
1702    2     Integer           LivesRemaining
1704    32    Integer[16]       unknown9
1736    20    String*19+Byte    GangName
1756    6     Byte[6]           unknown11
1762    2     Integer           CPRemaining
1764    8     String*8          SuicidePassword
1772    78    Integer[39]       unknown12
1850    60    Integer[30]       Ability[0-29]
1910    60    Integer[30]       AbilityModifier[0-29]
1970    14    Integer[7]        unknown13a-g
1984    4     Long              CharLife
1988    18    Integer[9]        unknown13
2006    1     Byte              Bitmask1
2007    1     Byte              Bitmask2
2008    1     Byte              TestFlag1
2009    1     Byte              TestFlag2
2010    2     Integer           unknown14
2012    16    Long[4]           unknown15
```

## Enumerations

### Item Types
| Value | Type |
|-------|------|
| 0 | (None/Other) |
| 1 | Weapon |
| 2 | Armor |
| 3 | Sign/Readable |
| 4 | Rope/Climbable |
| 5 | Light Source |
| 6 | Key |
| 7 | Container |
| 8 | Container (special) |
| 9 | Scroll/Parchment |
| 10 | Edible/Drinkable |

### Weapon Types (from Nightmare Redux frmItem.frm)
| Value | Type |
|-------|------|
| 0 | None |
| 1 | 1-Handed Blunt |
| 2 | 1-Handed Sharp |
| 3 | 2-Handed Blunt |
| 4 | 2-Handed Sharp |
| 5 | Thrown |

### Armor Types
| Value | Type |
|-------|------|
| 0 | None |
| 1 | Natural Armor |
| 2 | Robes/Clothes |
| 3 | Leather |
| 4 | Chain |
| 5 | Plate |

### Body Locations (WornOn)
| Value | Slot |
|-------|------|
| 0 | None |
| 1 | Head |
| 2 | Hands |
| 3 | Finger |
| 4 | Feet |
| 5 | Arms |
| 6 | Back |
| 7 | Neck |
| 8 | Legs |
| 9 | Waist |
| 10 | Torso |
| 11 | Off-Hand |
| 12 | Eyes |
| 13 | Ears |
| 14 | Wrist |

### Currency Types (CostType)
| Value | Currency |
|-------|----------|
| 0 | Copper |
| 1 | Silver |
| 2 | Gold |
| 3 | Platinum |
| 4 | Runic |

### Races (13)
1=Human, 2=Dwarf, 3=Gnome, 4=Halfling, 5=Elf, 6=Half-Elf,
7=Dark-Elf, 8=Half-Orc, 9=Goblin, 10=Half-Ogre, 11=Kang,
12=Nekojin, 13=Gaunt One

### Classes (15)
1=Warrior, 2=Witchunter, 3=Paladin, 4=Cleric, 5=Priest,
6=Missionary, 7=Ninja, 8=Thief, 9=Bard, 10=Gypsy,
11=Warlock, 12=Mage, 13=Druid, 14=Ranger, 15=Mystic

## Ability System

Items, monsters, spells, races, and classes all share the same ability system.
Each has paired arrays: AbilityA (ability ID) and AbilityB (ability value).

Key ability IDs (from Nightmare Redux):
| ID | Ability | Value meaning |
|----|---------|---------------|
| 12 | Summon | Monster number to summon |
| 42 | Learn Spell | Spell number learned |
| 43 | Cast Spell | Spell number cast |
| 116 | Alter BS Attack | Backstab modifier |
| 148 | Textblock | Textblock number to execute |
| 151 | End Cast | Spell number |
| 153 | Kill Spell | Spell number to remove |
| 155 | Death Text | Textblock number on death |

(Full ability table has 150+ entries in ability.mdb shipped with Nightmare Redux)

## Data Flow

```
WCC Btrieve Dats (1.11p)
    |
    | Nightmare Redux reads via wbtrv32.dll
    | Filters via multi-pass "In Game" cross-referencing
    | Drops descriptions, unknowns, textblock content
    v
MME Access .mdb Export
    |
    | MegaMUD imports subset
    | Further compresses (drops abilities beyond N slots,
    | replaces shop numbers with shop name strings,
    | drops race restrictions, spell negations, etc.)
    v
MegaMUD MDB2 Files (Items.md, Monsters.md, etc.)
    |
    | MajorSLOP reads via MDB2 parser
    v
Web Client (item/spell/monster browsers)
```
