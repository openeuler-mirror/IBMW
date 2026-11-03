#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# fastcdr_v2_to_v1.py - Post-process fastddsgen-generated CdrAux.ipp files
# to replace FastCDR v2 member-function calls with v1-compatible code.
#
# This script is run automatically by cmake after fastddsgen generates code.
#
# Transformations:
#   1. #include <fastcdr/CdrSizeCalculator.hpp> → our compat header
#   2. scdr.begin_serialize_type(...) → removed (no-op in v1)
#   3. scdr.end_serialize_type(...) → removed (no-op in v1)
#   4. cdr.deserialize_type(encoding, lambda) → sequential cdr >> field reads
#   5. cdr.get_cdr_version() → removed from conditional expressions
#   6. MemberId(N) << in serialize → kept (compat header handles via operator<<)

import re
import sys
import os
import tempfile


def transform_cdraux(content):
    """Transform a CdrAux.ipp file from FastCDR v2 API to v1-compatible code."""

    # 1. Replace CdrSizeCalculator include with our compat header
    content = content.replace(
        '#include <fastcdr/CdrSizeCalculator.hpp>',
        '#include "type_support/dds/fastcdr_v1_compat.h"'
    )
    # Older generated fixtures may already contain the former transport-local
    # include.  Normalize it while regenerating so installed consumers only
    # see the framework-owned TypeSupport path.
    content = content.replace(
        '#include "dds_types/util/fastcdr_v1_compat.h"',
        '#include "type_support/dds/fastcdr_v1_compat.h"')

    # 1b. Remove v2-only fixed_size_string.hpp include (not available in v1)
    # This header is included by generated .hpp files but typically unused.
    content = content.replace(
        '#include <fastcdr/cdr/fixed_size_string.hpp>',
        '// fixed_size_string.hpp removed (FastCDR v2 only, not used)'
    )

    # 2. Remove begin_serialize_type calls (can span multiple lines)
    # Pattern: scdr.begin_serialize_type(current_state,\n ... );
    content = re.sub(
        r'\s*\w+\.begin_serialize_type\s*\([^;]*\);\s*\n',
        '\n',
        content,
        flags=re.DOTALL
    )

    # 3. Remove end_serialize_type calls
    content = re.sub(
        r'\s*\w+\.end_serialize_type\s*\([^)]*\);\s*\n',
        '\n',
        content
    )

    # 4. Remove the Cdr::state line that's only used for begin/end_serialize_type
    # Pattern: eprosima::fastcdr::Cdr::state current_state(scdr);
    content = re.sub(
        r'\s*eprosima::fastcdr::Cdr::state\s+\w+\s*\(\s*\w+\s*\)\s*;\s*\n',
        '\n',
        content
    )

    # 5. Transform deserialize_type with lambda into sequential reads.
    # The generated pattern is:
    #   cdr.deserialize_type(eprosima::fastcdr::CdrVersion::XCDRv2 == cdr.get_cdr_version() ?
    #       eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2 :
    #       eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR,
    #       [&data](eprosima::fastcdr::Cdr& dcdr, const eprosima::fastcdr::MemberId& mid) -> bool
    #       {
    #           bool ret_value = true;
    #           switch (mid.id)
    #           {
    #               case 0: dcdr >> data.field1(); break;
    #               case 1: dcdr >> data.field2(); break;
    #               default: ret_value = false; break;
    #           }
    #           return ret_value;
    #       });
    #
    # We transform this to:
    #   eprosima::fastcdr::deserialize(cdr, data.field1());
    #   eprosima::fastcdr::deserialize(cdr, data.field2());
    #   ... (extracted from the case statements)

    content = transform_deserialize_type_blocks(content)

    # 6. Transform chained serialize statements:
    #   scdr << MemberId(0) << data.code() << MemberId(1) << data.msg();
    # into individual serialize() calls:
    #   eprosima::fastcdr::serialize(scdr, data.code());
    #   eprosima::fastcdr::serialize(scdr, data.msg());
    content = transform_chained_serialize(content)

    # 7. Transform individual scdr << expr; in serialize_key and other functions
    # into eprosima::fastcdr::serialize(scdr, expr);
    # But skip: scdr << eprosima::fastcdr::MemberId(N)  (standalone MemberId lines)
    content = transform_individual_serialize(content)

    # 8. Transform individual cdr >> expr; into eprosima::fastcdr::deserialize(cdr, expr);
    content = transform_individual_deserialize(content)

    # 9. Make all function definitions inline so .ipp can be safely included
    #    from multiple translation units without multiple-definition errors.
    content = make_functions_inline(content)

    return content


def make_functions_inline(content):
    """Add 'inline' to all function definitions in .ipp files.

    Explicit template specializations and regular functions in .ipp files
    have external linkage by default. When the .ipp is included from
    multiple translation units, this causes multiple-definition linker errors.

    We add 'inline' to:
    1. template<> ... eProsima_user_DllExport TYPE FUNC(...) { ... }
    2. Regular (non-template) function definitions like void serialize_key(...)

    We skip:
    - Lines that already have 'inline'
    - Pure declarations (no body / no '{' follows)
    """
    # Pattern 1: template<> specializations with eProsima_user_DllExport
    # e.g.: template<>\neProsima_user_DllExport size_t calculate_serialized_size(
    # Add inline after template<>\n
    content = re.sub(
        r'^(template\s*<>\s*\n)(eProsima_user_DllExport\s)',
        r'\1inline \2',
        content,
        flags=re.MULTILINE
    )

    # Pattern 2: Non-template functions like:
    # void serialize_key(\n    eprosima::fastcdr::Cdr& scdr, ...
    # These are regular functions in the eprosima::fastcdr namespace.
    # Add inline before 'void serialize_key('
    content = re.sub(
        r'^(void\s+serialize_key\s*\()',
        r'inline \1',
        content,
        flags=re.MULTILINE
    )

    return content


def transform_deserialize_type_blocks(content):
    """Find and transform all deserialize_type blocks."""
    # We need to find each deserialize_type call and extract the field reads
    # from the switch/case inside the lambda.

    result = []
    pos = 0

    while pos < len(content):
        # Find next deserialize_type call
        match = re.search(
            r'(\s*)(\w+)\.deserialize_type\s*\(',
            content[pos:]
        )
        if not match:
            result.append(content[pos:])
            break

        # Add everything before this match
        result.append(content[pos:pos + match.start()])
        indent = match.group(1)
        cdr_var = match.group(2)

        # Find the matching closing ");", accounting for nested braces
        start_paren = pos + match.start() + len(match.group(0)) - 1  # position of '('
        end_pos = find_matching_close(content, start_paren)

        if end_pos < 0:
            # Couldn't find matching close, keep original
            result.append(content[pos + match.start():])
            break

        # Extract the full deserialize_type(...) block
        block = content[start_paren:end_pos + 1]  # includes ( ... )

        # Find the lambda parameter name for the Cdr reference
        # Pattern: [&data](eprosima::fastcdr::Cdr& dcdr, ...
        lambda_match = re.search(
            r'\[\&\w+\]\s*\(\s*eprosima::fastcdr::Cdr\s*&\s*(\w+)',
            block
        )
        dcdr_var = lambda_match.group(1) if lambda_match else 'dcdr'

        # Extract field reads from case statements
        # Pattern: case N:\n  dcdr >> data.field(); break;
        # or:      case N: dcdr >> data.field(); break;
        field_reads = extract_field_reads(block, dcdr_var, cdr_var)

        if field_reads:
            result.append('\n')
            for read in field_reads:
                result.append(f'{indent}{read}\n')
        else:
            # Fallback: keep the original (shouldn't happen)
            result.append(content[pos + match.start():end_pos + 2])

        # Skip the trailing semicolon after )
        pos = end_pos + 1
        if pos < len(content) and content[pos] == ';':
            pos += 1

    return ''.join(result)


def find_matching_close(content, open_pos):
    """Find the matching closing paren/brace for the one at open_pos."""
    char = content[open_pos]
    if char == '(':
        close_char = ')'
    elif char == '{':
        close_char = '}'
    else:
        return -1

    depth = 1
    pos = open_pos + 1
    while pos < len(content) and depth > 0:
        c = content[pos]
        if c == char:
            depth += 1
        elif c == close_char:
            depth -= 1
        elif c == '"':
            # Skip string literals
            pos += 1
            while pos < len(content) and content[pos] != '"':
                if content[pos] == '\\':
                    pos += 1
                pos += 1
        elif c == "'":
            # Skip char literals
            pos += 1
            while pos < len(content) and content[pos] != "'":
                if content[pos] == '\\':
                    pos += 1
                pos += 1
        pos += 1

    if depth == 0:
        return pos - 1
    return -1


def extract_field_reads(block, dcdr_var, cdr_var):
    """Extract field reads from the switch/case inside deserialize_type lambda."""
    reads = []

    # Find all case statements with their reads
    # Patterns:
    #   case N: dcdr >> data.field(); break;
    #   case N: { uint32_t tmp; dcdr >> tmp; data.field = static_cast<...>(tmp); break; }
    #   case N:\n dcdr >> data.field();\n break;

    # First, try simple single-line reads: case N: dcdr >> expr; break;
    simple_cases = re.findall(
        r'case\s+\d+\s*:\s*\n?\s*' + re.escape(dcdr_var) + r'\s*>>\s*([^;]+)\s*;\s*\n?\s*break\s*;',
        block
    )

    if simple_cases:
        for field_expr in simple_cases:
            reads.append(f'eprosima::fastcdr::deserialize({cdr_var}, {field_expr.strip()});')
        return reads

    # Try multi-line case with braces (for enum casting etc.)
    # case N: { ... dcdr >> var; data.field = static_cast<...>(var); break; }
    brace_cases = re.findall(
        r'case\s+\d+\s*:\s*\{([^}]+)\}',
        block
    )

    for case_body in brace_cases:
        # Find dcdr >> var patterns
        read_match = re.search(
            re.escape(dcdr_var) + r'\s*>>\s*(\w+)\s*;',
            case_body
        )
        assign_match = re.search(
            r'(data\.\w+(?:\(\))?)\s*=\s*static_cast<[^>]+>\s*\((\w+)\)',
            case_body
        )

        if read_match and assign_match:
            # Enum case: read temp var, then cast
            temp_var = read_match.group(1)
            field = assign_match.group(1)
            cast_expr = re.search(r'static_cast<([^>]+)>', case_body).group(0)
            # Emit: { Type tmp; deserialize(cdr, tmp); data.field = cast(tmp); }
            type_match = re.search(r'(\w+)\s+' + re.escape(temp_var) + r'\s*;', case_body)
            if type_match:
                var_type = type_match.group(1)
                reads.append(f'{{ {var_type} _tmp; eprosima::fastcdr::deserialize({cdr_var}, _tmp); {field} = {cast_expr}(_tmp); }}')
            else:
                reads.append(f'eprosima::fastcdr::deserialize({cdr_var}, {field});')
        elif read_match:
            reads.append(f'eprosima::fastcdr::deserialize({cdr_var}, {read_match.group(1).strip()});')

    if reads:
        return reads

    # Last resort: find all dcdr >> patterns
    all_reads = re.findall(
        re.escape(dcdr_var) + r'\s*>>\s*([^;]+)\s*;',
        block
    )
    for r in all_reads:
        r = r.strip()
        if r and not r.startswith('data') and 'mid' not in r:
            reads.append(f'eprosima::fastcdr::deserialize({cdr_var}, {r});')
        elif r:
            reads.append(f'eprosima::fastcdr::deserialize({cdr_var}, {r});')

    return reads


def transform_chained_serialize(content):
    """Transform chained scdr << MemberId(N) << expr patterns.

    Input pattern (multi-line chained):
        scdr
            << eprosima::fastcdr::MemberId(0) << data.code()
            << eprosima::fastcdr::MemberId(1) << data.msg()
            << eprosima::fastcdr::MemberId(2) << data.data()
        ;

    Output:
        eprosima::fastcdr::serialize(scdr, data.code());
        eprosima::fastcdr::serialize(scdr, data.msg());
        eprosima::fastcdr::serialize(scdr, data.data());
    """
    # Match the full chained expression: starts with varname on its own line,
    # followed by << MemberId(N) << expr pairs, ending with ;
    # Pattern: <indent><varname>\n(<indent><< MemberId(N) << expr\n)+<indent>;
    def replace_chain(match):
        indent = match.group(1)
        cdr_var = match.group(2)
        chain_body = match.group(3)

        # Extract each MemberId(N) << expr pair
        # Pattern: << eprosima::fastcdr::MemberId(N) << <expr>
        pairs = re.findall(
            r'<<\s*eprosima::fastcdr::MemberId\(\d+\)\s*<<\s*(.+?)(?=\s*<<\s*eprosima::fastcdr::MemberId|\s*$)',
            chain_body,
            re.MULTILINE
        )

        if not pairs:
            return match.group(0)  # no change

        result_lines = []
        for expr in pairs:
            expr = expr.strip().rstrip(';').strip()
            if expr:
                result_lines.append(f'{indent}    eprosima::fastcdr::serialize({cdr_var}, {expr});')

        return '\n'.join(result_lines) + '\n'

    # Match: <indent><varname>\n<stuff with << MemberId ... >\n<indent>;
    content = re.sub(
        r'^([ \t]*)(\w+)\s*\n((?:[ \t]*<<\s*eprosima::fastcdr::MemberId\(\d+\)\s*<<[^\n]+\n)+)[ \t]*;[ \t]*\n',
        replace_chain,
        content,
        flags=re.MULTILINE
    )

    return content


def transform_individual_serialize(content):
    """Transform individual scdr << expr; statements to serialize() calls.

    Transforms:
        scdr << data.msg();
    To:
        eprosima::fastcdr::serialize(scdr, data.msg());

    Skips lines that are part of a chain (start with <<) or contain MemberId.
    Also skips static_cast<void> lines.
    """
    def replace_individual(match):
        indent = match.group(1)
        cdr_var = match.group(2)
        expr = match.group(3).strip()

        # Skip MemberId-only lines
        if 'MemberId' in expr:
            return match.group(0)

        # Skip static_cast<void> lines
        if 'static_cast<void>' in match.group(0):
            return match.group(0)

        return f'{indent}eprosima::fastcdr::serialize({cdr_var}, {expr});'

    # Match: <indent><varname> << <expr>;
    # But NOT lines that start with << (those are part of chains, already handled)
    content = re.sub(
        r'^([ \t]+)(\w+)\s*<<\s*([^<][^;]*);',
        replace_individual,
        content,
        flags=re.MULTILINE
    )

    return content


def transform_individual_deserialize(content):
    """Transform individual cdr >> expr; statements to deserialize() calls.

    Transforms:
        cdr >> data.code();
    To:
        eprosima::fastcdr::deserialize(cdr, data.code());
    """
    def replace_individual(match):
        indent = match.group(1)
        cdr_var = match.group(2)
        expr = match.group(3).strip()

        return f'{indent}eprosima::fastcdr::deserialize({cdr_var}, {expr});'

    # Match: <indent><varname> >> <expr>;
    content = re.sub(
        r'^([ \t]+)(\w+)\s*>>\s*([^;]+);',
        replace_individual,
        content,
        flags=re.MULTILINE
    )

    return content


def transform_pubsubtypes(content):
    """Transform a PubSubTypes.cxx file from FastCDR v2 API to v1-compatible code.

    The generated PubSubTypes.cxx files use:
      ser << *p_type;    (calls p_type->serialize(ser) in v1 — but IDL types lack that member)
      deser >> *p_type;  (calls p_type->deserialize(deser) — same problem)

    We replace these with explicit free-function calls that ARE provided by CdrAux.ipp:
      eprosima::fastcdr::serialize(ser, *p_type);
      eprosima::fastcdr::deserialize(deser, *p_type);
    """

    # 1. Replace CdrSizeCalculator include with our compat header (same as CdrAux)
    content = content.replace(
        '#include <fastcdr/CdrSizeCalculator.hpp>',
        '#include "type_support/dds/fastcdr_v1_compat.h"'
    )
    content = content.replace(
        '#include "dds_types/util/fastcdr_v1_compat.h"',
        '#include "type_support/dds/fastcdr_v1_compat.h"')

    # 2. Remove v2-only fixed_size_string.hpp include
    content = content.replace(
        '#include <fastcdr/cdr/fixed_size_string.hpp>',
        '// fixed_size_string.hpp removed (FastCDR v2 only, not used)'
    )

    # 3. Replace  ser << *p_type;  with  eprosima::fastcdr::serialize(ser, *p_type);
    # Pattern: <whitespace><varname> << *<varname>;
    content = re.sub(
        r'(\s+)(\w+)\s*<<\s*\*(\w+)\s*;',
        r'\1eprosima::fastcdr::serialize(\2, *\3);',
        content
    )

    # 4. Replace  deser >> *p_type;  with  eprosima::fastcdr::deserialize(deser, *p_type);
    content = re.sub(
        r'(\s+)(\w+)\s*>>\s*\*(\w+)\s*;',
        r'\1eprosima::fastcdr::deserialize(\2, *\3);',
        content
    )

    # 5. Replace v2 member function calls that may appear in PubSubTypes.cxx
    # ser.get_serialized_data_length() → ser.getSerializedDataLength()
    content = re.sub(
        r'(\w+)\.get_serialized_data_length\(\)',
        r'\1.getSerializedDataLength()',
        content
    )

    # 6. Remove begin_serialize_type / end_serialize_type calls (same as CdrAux)
    content = re.sub(
        r'\s*\w+\.begin_serialize_type\s*\([^;]*\);\s*\n',
        '\n',
        content,
        flags=re.DOTALL
    )
    content = re.sub(
        r'\s*\w+\.end_serialize_type\s*\([^)]*\);\s*\n',
        '\n',
        content
    )

    # 7. Remove Cdr::state lines
    content = re.sub(
        r'\s*eprosima::fastcdr::Cdr::state\s+\w+\s*\(\s*\w+\s*\)\s*;\s*\n',
        '\n',
        content
    )

    return content


def inject_dependent_ipp_includes(content, filepath, upstream_dirs=None):
    """For a CdrAux.ipp file, detect dependent CdrAux.ipp files and add includes.

    When ExampleRpcCdrAux.ipp serializes fields that are complex types (e.g.
    ExampleFoo), it calls eprosima::fastcdr::serialize(cdr, data.data()) which
    needs the ExampleCommonCdrAux.ipp specialization to be visible.

    Detection approach:
    1. Find #include "XxxCdrAux.hpp" in this .ipp file
    2. Read that .hpp file
    3. Find includes like #include "Yyy.hpp" (type definition headers from other IDL files)
    4. Check if YyyCdrAux.ipp exists in the same directory or any upstream gencode dir
    5. If yes, add #include "YyyCdrAux.ipp" before the existing CdrAux.hpp include

    `upstream_dirs` is an optional list of additional gencode directories to
    search for dependent CdrAux.ipp files (used when an IDL package depends on
    types defined in a separately-generated package, e.g. sensor -> common).
    """
    if not filepath.endswith('CdrAux.ipp'):
        return content

    ipp_dir = os.path.dirname(filepath)
    search_dirs = [ipp_dir] + list(upstream_dirs or [])

    # Find the CdrAux.hpp include in this file
    hpp_match = re.search(r'#include\s+"(\w+CdrAux\.hpp)"', content)
    if not hpp_match:
        return content

    cdraux_hpp_name = hpp_match.group(1)
    cdraux_hpp_path = os.path.join(ipp_dir, cdraux_hpp_name)

    if not os.path.exists(cdraux_hpp_path):
        return content

    # Read the CdrAux.hpp to find the type definition .hpp include
    with open(cdraux_hpp_path, 'r') as f:
        hpp_content = f.read()

    # Find includes like #include "ExampleRpc.hpp" (the type definition header)
    type_hpp_match = re.search(r'#include\s+"(\w+)\.hpp"', hpp_content)
    if not type_hpp_match:
        return content

    type_hpp_name = type_hpp_match.group(1)  # e.g. "ExampleRpc"
    type_hpp_path = os.path.join(ipp_dir, type_hpp_name + '.hpp')

    if not os.path.exists(type_hpp_path):
        return content

    # Read the type definition .hpp to find dependent .hpp includes
    with open(type_hpp_path, 'r') as f:
        type_hpp_content = f.read()

    # Find includes of other type definition headers (any of the search dirs)
    dep_includes = re.findall(r'#include\s+"(\w+)\.hpp"', type_hpp_content)

    # For each dependency, check if a CdrAux.ipp exists in any search dir and add include
    dep_ipp_includes = []
    own_ipp_basename = os.path.basename(filepath)
    for dep_name in dep_includes:
        dep_ipp = dep_name + 'CdrAux.ipp'
        if dep_ipp == own_ipp_basename:
            continue
        for d in search_dirs:
            if os.path.exists(os.path.join(d, dep_ipp)):
                inc = f'#include "{dep_ipp}"'
                if inc not in dep_ipp_includes:
                    dep_ipp_includes.append(inc)
                break

    if not dep_ipp_includes:
        return content

    # Insert the dependent .ipp includes BEFORE the own CdrAux.hpp include
    # This ensures dependent specializations are visible when this .ipp is parsed
    insert_text = '\n'.join(dep_ipp_includes) + '\n'
    content = content.replace(
        f'#include "{cdraux_hpp_name}"',
        insert_text + f'#include "{cdraux_hpp_name}"'
    )

    print(f"  Injected dependent .ipp includes: {dep_ipp_includes}")
    return content


def write_atomic(filepath, text):
    """Replaces filepath with text; an interrupted build leaves it unchanged."""
    fd, tmp = tempfile.mkstemp(prefix='.fastcdr-', dir=os.path.dirname(os.path.abspath(filepath)))
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(text)
        os.chmod(tmp, os.stat(filepath).st_mode & 0o7777)
        os.replace(tmp, filepath)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} [--upstream-dir <dir>]... <file.ipp|.cxx|.hpp> [file2 ...]", file=sys.stderr)
        sys.exit(1)

    # Parse optional --upstream-dir <dir> args (may appear multiple times) before
    # the positional file list. These are extra gencode directories searched
    # when injecting cross-package CdrAux.ipp dependencies.
    upstream_dirs = []
    args = sys.argv[1:]
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--upstream-dir':
            if i + 1 >= len(args):
                print("Error: --upstream-dir needs a directory argument", file=sys.stderr)
                sys.exit(1)
            upstream_dirs.append(args[i + 1])
            i += 2
        else:
            files.append(a)
            i += 1

    # Every input is an output of fastddsgen: a missing one is an error, found
    # before anything is changed.
    missing = [f for f in files if not os.path.isfile(f)]
    if missing:
        for filepath in missing:
            print(f"Error: {filepath} not found", file=sys.stderr)
        sys.exit(1)

    for filepath in files:
        with open(filepath, 'r') as f:
            content = f.read()

        if filepath.endswith('PubSubTypes.cxx'):
            transformed = transform_pubsubtypes(content)
        elif filepath.endswith('.hpp'):
            # For .hpp files, only remove fixed_size_string include
            transformed = transform_cdraux(content)
        elif filepath.endswith('CdrAux.ipp'):
            transformed = transform_cdraux(content)
            transformed = inject_dependent_ipp_includes(transformed, filepath, upstream_dirs)
        else:
            transformed = transform_cdraux(content)

        write_atomic(filepath, transformed)

        print(f"Transformed: {filepath}")


if __name__ == '__main__':
    main()
