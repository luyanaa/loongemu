#!/usr/bin/env python3
"""Generate typed IR1 capture wrappers and rejecting trans stubs.

The decoder prototypes and argument layouts come from QEMU decodetree output.
Only names listed in rv64gc-opcodes.def are accepted by the LATR front end;
all other QEMU extensions fall through as illegal instructions.
"""
import re
import sys
from pathlib import Path


def parse_fields(paths):
    fields = {}
    aliases = {}
    struct_re = re.compile(r"typedef\s+struct\s*\{(.*?)\}\s*arg_(\w+)\s*;", re.S)
    field_re = re.compile(r"\bint\s+(\w+)\s*;")
    alias_re = re.compile(r"typedef\s+arg_(\w+)\s+arg_(\w+)\s*;")
    for path in paths:
        text = Path(path).read_text()
        for body, name in struct_re.findall(text):
            fields[name] = field_re.findall(body)
        aliases.update((name, base) for base, name in alias_re.findall(text))
    for name in aliases:
        base = aliases[name]
        seen = {name}
        while base in aliases:
            if base in seen:
                raise ValueError(f"cyclic arg typedef: {name}")
            seen.add(base)
            base = aliases[base]
        if base not in fields:
            raise ValueError(f"missing arg layout for arg_{name} (base arg_{base})")
        fields[name] = fields[base]
    return fields


def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: gen_decode_stubs.py insn32.inc insn16.inc opcodes.def output.inc")
    decoder_paths = sys.argv[1:3]
    opdef_path, output_path = sys.argv[3:]
    arg_fields = parse_fields(decoder_paths)
    mapping = {}
    for line in Path(opdef_path).read_text().splitlines():
        match = re.match(r"\s*LATR_RV_(?:INSN|ALIAS)\((\w+)\s*,\s*(\w+)\s*\)", line)
        if match:
            tag, name = match.groups()
            if name in mapping:
                raise ValueError(f"duplicate decoder name {name}")
            mapping[name] = tag

    prototypes = {}
    prototype_re = re.compile(
        r"^static bool trans_(\w+)\(DisasContext \*ctx, arg_(\w+) \*a\);$", re.M
    )
    for path in decoder_paths:
        for name, arg_name in prototype_re.findall(Path(path).read_text()):
            if name in prototypes and prototypes[name] != arg_name:
                raise ValueError(f"conflicting prototypes for trans_{name}")
            prototypes[name] = arg_name

    ir1_fields = {"rd", "rs1", "rs2", "rs3", "imm", "shamt", "csr", "zimm",
                  "rm", "aq", "rl", "pred", "succ"}
    out = ["/* Generated from QEMU decodetree declarations; do not edit. */"]
    for name, arg_name in sorted(prototypes.items()):
        if arg_name not in arg_fields:
            raise ValueError(f"missing field layout for trans_{name}: arg_{arg_name}")
        out.append(f"static bool trans_{name}(DisasContext *ctx, arg_{arg_name} *a)")
        out.append("{")
        if name in mapping:
            out.append(f"    ctx->ir1->opcode = LATR_RV_{mapping[name]};")
            for field in arg_fields[arg_name]:
                if field not in ir1_fields:
                    raise ValueError(f"unmapped field {field} on supported trans_{name}")
                out.append(f"    ctx->ir1->arg.{field} = a->{field};")
            out.append("    return true;")
        elif name in {"illegal", "c64_illegal"}:
            out.append("    (void)a;")
            out.append("    ctx->ir1->opcode = LATR_RV_ILLEGAL;")
            out.append("    return true;")
        else:
            out.append("    (void)ctx;")
            out.append("    (void)a;")
            out.append("    return false;")
        out.append("}")
    Path(output_path).write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
