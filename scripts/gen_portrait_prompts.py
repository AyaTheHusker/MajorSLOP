"""Generate portrait prompts for every Race × Class × Gender combination.

MajorMUD character appearance is determined by:
- Race (13 races)
- Class (15 classes)
- Gender (male/female)
- Stat descriptors (physically godlike, frail, etc.)

This generates a JSON of all valid combos with curated image-gen prompts.
"""
import json
import itertools
from pathlib import Path

# ── Race lore & appearance ──
RACES = {
    "Human": {
        "desc": "Average height and build, versatile and adaptable",
        "look": "human, medium build, varied features",
        "skin": ["fair", "tan", "olive", "brown", "dark"],
        "hair": ["brown", "black", "blonde", "red", "auburn", "grey"],
        "eyes": ["brown", "blue", "green", "hazel", "grey"],
        "height": "average height",
        "build": "medium build",
    },
    "Dwarf": {
        "desc": "Short and stocky, incredibly tough, natural miners and smiths",
        "look": "dwarf, short and stocky, thick powerful limbs, broad shoulders",
        "skin": ["ruddy", "tan", "weathered brown"],
        "hair": ["brown", "black", "red", "grey", "white"],
        "eyes": ["brown", "grey", "dark blue"],
        "height": "short, about four feet tall",
        "build": "stocky and barrel-chested, powerfully built",
    },
    "Gnome": {
        "desc": "Small and clever, naturally stealthy, quick and nimble",
        "look": "gnome, small and wiry, sharp clever features, large pointed ears",
        "skin": ["tan", "ruddy", "earthy brown"],
        "hair": ["white", "grey", "brown", "sandy"],
        "eyes": ["bright blue", "green", "grey"],
        "height": "very small, about three and a half feet tall",
        "build": "small and wiry, nimble",
    },
    "Halfling": {
        "desc": "Very small and incredibly agile, natural rogues and entertainers",
        "look": "halfling, very small, round friendly face, curly hair, bare hairy feet",
        "skin": ["tan", "ruddy", "fair"],
        "hair": ["brown", "sandy", "curly brown", "auburn"],
        "eyes": ["brown", "hazel", "green"],
        "height": "very short, barely three feet tall",
        "build": "small and nimble, surprisingly agile",
    },
    "Elf": {
        "desc": "Tall and slender, graceful and intelligent, ancient and magical",
        "look": "elf, tall and slender, angular features, pointed ears, ethereal beauty",
        "skin": ["pale", "fair", "alabaster", "light golden"],
        "hair": ["silver", "golden blonde", "platinum", "raven black", "copper"],
        "eyes": ["bright green", "ice blue", "violet", "amber", "silver"],
        "height": "tall and willowy",
        "build": "slender and graceful, lithe",
    },
    "Half-Elf": {
        "desc": "Mix of human and elven heritage, graceful yet sturdy, charismatic",
        "look": "half-elf, slightly pointed ears, blend of human and elven features, attractive",
        "skin": ["fair", "tan", "light olive"],
        "hair": ["brown", "blonde", "auburn", "black", "copper"],
        "eyes": ["green", "blue", "hazel", "amber"],
        "height": "slightly taller than average",
        "build": "lean and graceful",
    },
    "Dark-Elf": {
        "desc": "Subterranean elf, dark skin, white hair, masters of stealth and magic",
        "look": "dark elf, obsidian black skin, stark white hair, pointed ears, fierce angular features",
        "skin": ["obsidian black", "deep charcoal", "dark grey-purple"],
        "hair": ["stark white", "silver-white", "pale silver"],
        "eyes": ["crimson red", "pale violet", "amber", "white"],
        "height": "tall and lean",
        "build": "wiry and agile, predatory grace",
    },
    "Half-Orc": {
        "desc": "Strong and tough, mix of human and orcish blood, fierce fighters",
        "look": "half-orc, muscular, slightly greenish skin, prominent jaw, small tusks",
        "skin": ["greenish grey", "olive green", "sallow grey-green"],
        "hair": ["black", "dark brown", "coarse black"],
        "eyes": ["yellow", "red-brown", "dark brown"],
        "height": "tall and imposing",
        "build": "heavily muscled, broad and powerful",
    },
    "Goblin": {
        "desc": "Small, quick and cunning, natural thieves, surprisingly agile",
        "look": "goblin, small green-skinned humanoid, large pointed ears, sharp features, beady eyes",
        "skin": ["pale green", "dark green", "mottled green-brown"],
        "hair": ["black", "dark green", "none"],
        "eyes": ["yellow", "red", "orange"],
        "height": "small, about three feet tall",
        "build": "scrawny but quick, wiry",
    },
    "Half-Ogre": {
        "desc": "Massive and incredibly strong, towering brutes, low intelligence but devastating power",
        "look": "half-ogre, massive hulking humanoid, thick brutish features, enormous muscles",
        "skin": ["yellowish brown", "grey", "dull brown"],
        "hair": ["black", "dark brown", "greasy black"],
        "eyes": ["dull brown", "black", "yellow"],
        "height": "towering, over seven feet tall",
        "build": "massive and hulking, enormously muscled",
    },
    "Kang": {
        "desc": "Lizard-like reptilian humanoid, scaled skin, forked tongue, poison immune, natural damage resistance",
        "look": "kang, reptilian humanoid, scaled skin, snake-like eyes, forked tongue, tail",
        "skin": ["dark green scales", "mottled brown-green scales", "deep emerald scales", "dark olive scales"],
        "hair": ["none", "ridge of spines"],
        "eyes": ["yellow slit-pupil", "amber reptilian", "green slit-pupil"],
        "height": "tall and powerfully built",
        "build": "muscular and scaled, powerful reptilian frame",
    },
    "Nekojin": {
        "desc": "Cat-like humanoid, feline features, extremely agile, graceful hunters",
        "look": "nekojin, beautiful anime-style cat-eared humanoid, soft feline features, cat ears, slitted eyes, small fangs, graceful",
        "skin": ["tawny fur", "black fur", "spotted fur", "grey tabby fur", "white fur"],
        "hair": ["matching fur color, mane-like"],
        "eyes": ["golden slitted", "green slitted", "blue slitted", "amber cat-eyes"],
        "height": "average to tall",
        "build": "lithe and agile, feline grace, lean muscular",
    },
    "Gaunt One": {
        "desc": "Ancient, ethereal beings, extremely intelligent but frail, powerful magic users, lanky and skeletal",
        "look": "gaunt one, extremely tall and skeletal, sunken features, elongated limbs, otherworldly aura",
        "skin": ["deathly pale", "grey-white", "translucent pallid"],
        "hair": ["thin white wisps", "none", "wispy grey"],
        "eyes": ["glowing pale blue", "milky white", "luminous grey"],
        "height": "very tall and unnaturally thin",
        "build": "skeletal and gaunt, eerily thin, elongated limbs",
    },
}

# ── Class lore & equipment ──
CLASSES = {
    "Warrior": {
        "desc": "Master of arms, heavy armor and weapons, frontline fighter",
        "armor": "heavy plate armor, full helm",
        "weapon": "greatsword, battleaxe, or warhammer",
        "magic": None,
        "vibe": "battle-hardened veteran, confident martial stance",
    },
    "Witchunter": {
        "desc": "Elite anti-magic warrior, hunts spellcasters, heavily armored",
        "armor": "dark heavy armor with anti-magic sigils and runes",
        "weapon": "runed bastard sword or flail",
        "magic": None,
        "vibe": "grim inquisitor, cold determined expression, intimidating presence",
    },
    "Paladin": {
        "desc": "Holy warrior, divine magic and heavy combat, champion of justice",
        "armor": "shining plate armor with holy symbols, tabard",
        "weapon": "blessed longsword and shield, or holy mace",
        "magic": "divine",
        "vibe": "noble and righteous, radiant holy aura, resolute expression",
    },
    "Cleric": {
        "desc": "Divine healer and warrior-priest, moderate combat with strong divine magic",
        "armor": "heavy chainmail or plate with religious vestments",
        "weapon": "mace, warhammer, or flail",
        "magic": "divine",
        "vibe": "pious and stern, carries holy symbol, emanates divine authority",
    },
    "Priest": {
        "desc": "Pure divine caster, most powerful healing and holy magic, no armor or weapons",
        "armor": "flowing white and gold robes, religious vestments",
        "weapon": "ornate staff or holy symbol",
        "magic": "divine",
        "vibe": "serene and contemplative, glowing with holy light, priestly bearing",
    },
    "Missionary": {
        "desc": "Traveling healer and divine caster, light armor, stealthy priest",
        "armor": "worn leather traveling clothes with religious symbols",
        "weapon": "walking staff or light mace",
        "magic": "divine",
        "vibe": "humble traveler, weathered but kind, wandering healer",
    },
    "Ninja": {
        "desc": "Shadow assassin, master of stealth, deadly critical strikes, light armor",
        "armor": "dark fitted cloth wrappings, ninja garb, face mask",
        "weapon": "katana, wakizashi, throwing stars, or ninjato",
        "magic": None,
        "vibe": "deadly and silent, crouched in shadows, piercing calculating eyes",
    },
    "Thief": {
        "desc": "Master of stealth and lockpicking, backstab specialist, light armor",
        "armor": "dark leather armor, hooded cloak, many pouches and tools",
        "weapon": "dagger, short sword, or rapier",
        "magic": None,
        "vibe": "sly and alert, lurking in shadows, quick hands, cunning smirk",
    },
    "Bard": {
        "desc": "Musical spellcaster and entertainer, jack of all trades, charm magic",
        "armor": "colorful leather armor, travelling clothes, instrument on back",
        "weapon": "rapier, short sword, lute or flute",
        "magic": "bardic",
        "vibe": "charismatic performer, confident smile, musical instrument visible",
    },
    "Gypsy": {
        "desc": "Wandering fortune-teller with arcane magic, stealthy and mysterious",
        "armor": "layered exotic robes, scarves, jewelry, fortune-telling tools",
        "weapon": "crystal-topped staff or curved dagger",
        "magic": "arcane",
        "vibe": "mysterious and alluring, draped in scarves and trinkets, knowing eyes",
    },
    "Warlock": {
        "desc": "Dark combat mage, medium armor with offensive arcane magic",
        "armor": "dark leather and chain armor with arcane symbols, dark robes over armor",
        "weapon": "staff crackling with dark energy, or enchanted blade",
        "magic": "arcane",
        "vibe": "dark and brooding, arcane energy crackling around hands, intense gaze",
    },
    "Mage": {
        "desc": "Pure arcane caster, most powerful offensive magic, no armor or weapons",
        "armor": "ornate wizard robes with arcane symbols, pointed hat or hood",
        "weapon": "gnarled staff with glowing crystal, or spellbook",
        "magic": "arcane",
        "vibe": "scholarly and powerful, arcane runes floating nearby, wise piercing gaze",
    },
    "Druid": {
        "desc": "Nature magic master, shapeshifting, powerful nature spells, moderate armor",
        "armor": "natural leather and hide armor, adorned with leaves and vines, antlers",
        "weapon": "gnarled wooden staff, sickle, or nature-infused mace",
        "magic": "nature",
        "vibe": "wild and primal, one with nature, leaves and vines in hair, glowing green eyes",
    },
    "Ranger": {
        "desc": "Wilderness warrior-scout, nature magic, tracking, moderate armor",
        "armor": "forest green leather armor, hooded cloak, ranger's gear",
        "weapon": "longbow and hunting knife, or dual short swords",
        "magic": "nature",
        "vibe": "rugged outdoorsman, keen watchful eyes, weathered from the wild",
    },
    "Mystic": {
        "desc": "Martial arts master, unarmed combat, monk-like discipline, no armor",
        "armor": "simple monk robes or wrapped cloth, bare arms showing martial conditioning",
        "weapon": "bare fists, or bo staff",
        "magic": "monk ki",
        "vibe": "calm disciplined warrior-monk, meditative poise, coiled lethal energy",
    },
}

# MajorMUD stat-based appearance descriptors (from 'look' command)
STAT_DESCRIPTORS = {
    "strength": [
        (0, 30, "looks extremely frail and weak"),
        (31, 50, "looks rather weak"),
        (51, 70, "has an average build"),
        (71, 90, "looks fairly strong"),
        (91, 120, "looks very strong and muscular"),
        (121, 150, "has a powerful, imposing physique"),
        (151, 200, "is physically godlike"),
    ],
}

GENDERS = ["male", "female"]


def build_prompt(race_name, race_data, class_name, class_data, gender):
    """Build a curated portrait prompt for a Race/Class/Gender combo."""
    g = gender
    pronoun = "he" if g == "male" else "she"

    # Core description
    parts = [
        f"Fantasy character portrait, 9:16 aspect ratio, dramatic lighting,",
        f"{g} {race_data['look']},",
        f"{race_data['height']}, {race_data['build']},",
        f"wearing {class_data['armor']},",
        f"wielding {class_data['weapon']},",
        f"{class_data['vibe']},",
    ]

    # Magic aura
    if class_data['magic']:
        magic_map = {
            'divine': 'soft golden holy light emanating',
            'arcane': 'crackling arcane energy and mystical runes',
            'nature': 'swirling leaves and green nature magic aura',
            'bardic': 'shimmering musical notes floating in the air',
            'monk ki': 'subtle inner energy glow, disciplined aura',
        }
        parts.append(f"{magic_map.get(class_data['magic'], '')},")

    parts.append("detailed fantasy art, painterly style, dark atmospheric background")

    return " ".join(parts)


def main():
    races = json.load(open(Path(__file__).parent.parent / "dist" / "gamedata" / "races.json"))
    classes = json.load(open(Path(__file__).parent.parent / "dist" / "gamedata" / "classes.json"))

    race_names = [r["Name"] for r in races.values()]
    class_names = [c["Name"] for c in classes.values()]

    combos = []
    for race_name in race_names:
        if race_name not in RACES:
            print(f"WARNING: No lore data for race '{race_name}', skipping")
            continue
        race_data = RACES[race_name]

        for class_name in class_names:
            if class_name not in CLASSES:
                print(f"WARNING: No lore data for class '{class_name}', skipping")
                continue
            class_data = CLASSES[class_name]

            for gender in GENDERS:
                prompt = build_prompt(race_name, race_data, class_name, class_data, gender)
                key = f"{race_name}_{class_name}_{gender}".lower().replace(" ", "_").replace("-", "_")

                combos.append({
                    "key": key,
                    "race": race_name,
                    "class": class_name,
                    "gender": gender,
                    "prompt": prompt,
                    "skin_variants": race_data["skin"],
                    "hair_variants": race_data["hair"],
                    "eye_variants": race_data["eyes"],
                })

    # Stats
    print(f"Races: {len(race_names)}")
    print(f"Classes: {len(class_names)}")
    print(f"Genders: {len(GENDERS)}")
    print(f"Total combos: {len(combos)}")
    print(f"  = {len(race_names)} × {len(class_names)} × {len(GENDERS)}")

    # With skin/hair/eye variants for full expansion
    total_variants = sum(
        len(c["skin_variants"]) * len(c["hair_variants"]) * len(c["eye_variants"])
        for c in combos
    )
    print(f"Total with appearance variants: {total_variants}")
    print(f"  (that's a LOT — base combos of {len(combos)} are enough for v1)")

    # Write output
    outpath = Path(__file__).parent.parent / "dist" / "gamedata" / "portrait_prompts.json"
    outpath.parent.mkdir(parents=True, exist_ok=True)
    outpath.write_text(json.dumps(combos, indent=2))
    print(f"\nWritten to {outpath}")

    # Show a few examples
    print(f"\n── Example prompts ──")
    examples = [
        ("Human", "Warrior", "male"),
        ("Dark-Elf", "Ninja", "female"),
        ("Kang", "Cleric", "male"),
        ("Gaunt One", "Mage", "female"),
        ("Halfling", "Bard", "male"),
        ("Nekojin", "Ranger", "female"),
    ]
    for r, c, g in examples:
        for combo in combos:
            if combo["race"] == r and combo["class"] == c and combo["gender"] == g:
                print(f"\n[{combo['key']}]")
                print(combo["prompt"])
                break


if __name__ == "__main__":
    main()
