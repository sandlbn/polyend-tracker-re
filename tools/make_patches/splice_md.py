"""PRESETS.md while some preset modules are not in profile full: the PROFILE=full
markdown (every Mini model) plus the sections of the other profiles' markdown that
full does not have (nofm: JU6, D-Type; classic: CS-Type, A-Type; techno: T-Type), in their order,
each section once (the first profile that has it).

  python3 -I splice_md.py <full.md> <profile>=<profile.md> ...   -> PRESETS.md text on stdout
"""
import re
import sys


SECTION_RE = re.compile(r"\n## (.+?) \((\d+)\)\n")


def sections(text):
    """[(name, count, text)] of the '## name (n)' sections, up to 'Total:'."""
    end = text.index("\nTotal:")
    hits = list(SECTION_RE.finditer(text, 0, end))
    return [(m.group(1), int(m.group(2)), text[m.start():hits[i + 1].start() if i + 1 < len(hits) else end])
            for i, m in enumerate(hits)]


def join(names):
    return ", ".join(names[:-1]) + " and " + names[-1] if len(names) > 1 else names[0]


def main(full_path, others):
    full = open(full_path).read()
    have = {name for name, _, _ in sections(full)}
    extra = []    # (name, count, text)
    origin = []   # (profile, [names])
    for arg in others:
        profile, path = arg.split("=", 1)
        names = []
        for name, n, sec in sections(open(path).read()):
            if name in have:
                continue
            have.add(name)
            extra.append((name, n, sec))
            names.append(name)
        if names:
            origin.append((profile, names))
    if not extra:   # every preset module is in full: nothing to splice
        sys.stdout.write(full)
        return
    total = int(re.search(r"Total: (\d+) patches", full).group(1)) + sum(n for _, n, _ in extra)
    out = full[:full.index("\nTotal:")] + "".join(sec for _, _, sec in extra) + f"\nTotal: {total} patches.\n"
    parts = []
    for k, (profile, names) in enumerate(origin):
        many = len(names) > 1
        verb = (" come" if many else " comes") if k == 0 else ""
        parts.append(f"the {join(names)} section{'s' if many else ''}{verb} from `PROFILE={profile}`")
    line = ", ".join(parts)
    line = line[0].upper() + line[1:]
    out = out.replace("no factory patch is used.\n",
                      f"no factory patch is used.\n{line} (not in profile `full`).\n", 1)
    sys.stdout.write(out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
