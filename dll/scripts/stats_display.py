"""
Stats Display — Shows current player stats as floating text (VFT).

Usage:
    mud.load('stats_display')
"""
import time

CLASS_NAMES = {
    0: "Unknown", 1: "Warrior", 2: "Witchunter", 3: "Cleric",
    4: "Thief", 5: "Mage", 6: "Ganbusher", 7: "Bard",
    8: "Missionary", 9: "Ranger", 10: "Paladin", 11: "Warlock",
    12: "Ninja", 13: "Druid"
}

RACE_NAMES = {
    0: "Unknown", 1: "Human", 2: "Dwarf", 3: "Gnome",
    4: "Halfling", 5: "Elf", 6: "Half-Elf", 7: "Dark-Elf",
    8: "Half-Orc", 9: "Goblin"
}

name = mud.player_name()
level = mud.level()
race_id = mud.read_i32(OFF.PLAYER_RACE)
class_id = mud.read_i32(OFF.PLAYER_CLASS)
cur_hp, max_hp = mud.hp()
cur_mp, max_mp = mud.mana()

exp_lo = mud.read_i32(OFF.EXP_LO)
exp_hi = mud.read_i32(OFF.EXP_HI)
exp = exp_lo + (exp_hi << 32)

exp_need_lo = mud.read_i32(OFF.EXP_NEED_LO)
exp_need_hi = mud.read_i32(OFF.EXP_NEED_HI)
exp_need = exp_need_lo + (exp_need_hi << 32)

race = RACE_NAMES.get(race_id, "Unknown")
cls = CLASS_NAMES.get(class_id, "Unknown")

strength = mud.strength()
agility = mud.agility()
intellect = mud.intellect()
health = mud.health()
charm = mud.charm()

# Display as staggered VFT floating texts
# VFT uses 0-1 normalized coords; ~0.35 x, stepping y down
y = 0.15
step = 0.06
delay = 0.3
base = "intro=fade:0.2|hold=4|outro=fade:0.3"

mud.vft(f"{name}|0.35|{y}|size=4|color=FFDC50|{base}|shadow=1")
time.sleep(delay)
y += step

mud.vft(f"Level {level} {race} {cls}|0.35|{y}|size=3|color=B4DCFF|{base}|shadow=1")
time.sleep(delay)
y += step

hp_pct = (cur_hp * 100 // max_hp) if max_hp > 0 else 0
hp_color = "FF3C50" if hp_pct < 50 else "50FF50"
mud.vft(f"HP: {cur_hp}/{max_hp}|0.35|{y}|size=2.5|color={hp_color}|{base}|shadow=1")
time.sleep(delay)
y += step

mp_pct = (cur_mp * 100 // max_mp) if max_mp > 0 else 0
mp_color = "C88CFF" if mp_pct < 50 else "648CFF"
mud.vft(f"Mana: {cur_mp}/{max_mp}|0.35|{y}|size=2.5|color={mp_color}|{base}|shadow=1")
time.sleep(delay)
y += step

if exp_need > 0:
    mud.vft(f"EXP: {exp:,}  Need: {exp_need:,}|0.35|{y}|size=2|color=C8C8C8|{base}|shadow=1")
else:
    mud.vft(f"EXP: {exp:,}|0.35|{y}|size=2|color=C8C8C8|{base}|shadow=1")
time.sleep(delay)
y += step

mud.vft(f"STR {strength}  AGI {agility}  INT {intellect}  HP {health}  CHR {charm}|0.35|{y}|size=1.8|color=A0B4A0|{base}|shadow=1")

print(f"Stats displayed for {name}")
