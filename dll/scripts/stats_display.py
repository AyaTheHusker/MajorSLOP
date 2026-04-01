"""
Stats Display — Shows current player stats as floating text.

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

# Display as staggered floating texts
y = 120
delay = 0.3

mud.float_text(f"{name}", 255, 220, 80, duration=120, size=42, x=400, y=y)
time.sleep(delay)
y += 40

mud.float_text(f"Level {level} {race} {cls}", 180, 220, 255, duration=110, size=32, x=400, y=y)
time.sleep(delay)
y += 35

hp_pct = (cur_hp * 100 // max_hp) if max_hp > 0 else 0
hp_r = 255 if hp_pct < 50 else 80
hp_g = 255 if hp_pct >= 50 else 80
mud.float_text(f"HP: {cur_hp}/{max_hp}", hp_r, hp_g, 80, duration=100, size=30, x=400, y=y)
time.sleep(delay)
y += 30

mp_pct = (cur_mp * 100 // max_mp) if max_mp > 0 else 0
mp_b = 255
mp_r = 200 if mp_pct < 50 else 100
mud.float_text(f"Mana: {cur_mp}/{max_mp}", mp_r, 140, mp_b, duration=100, size=30, x=400, y=y)
time.sleep(delay)
y += 35

if exp_need > 0:
    mud.float_text(f"EXP: {exp:,}  Need: {exp_need:,}", 200, 200, 200, duration=100, size=26, x=400, y=y)
else:
    mud.float_text(f"EXP: {exp:,}", 200, 200, 200, duration=100, size=26, x=400, y=y)
time.sleep(delay)
y += 35

mud.float_text(f"STR {strength}  AGI {agility}  INT {intellect}  HP {health}  CHR {charm}",
               160, 180, 160, duration=100, size=22, x=400, y=y)

print(f"Stats displayed for {name}")
