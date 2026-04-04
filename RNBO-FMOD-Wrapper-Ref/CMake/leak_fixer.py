import sys
import os
import re

def _find_closing_brace(content: str, start_index: int) -> int:
    """Finds the matching closing brace for an opening brace at start_index."""
    if content[start_index] != '{':
        return -1
    
    brace_level = 1
    for i in range(start_index + 1, len(content)):
        char = content[i]
        if char == '{':
            brace_level += 1
        elif char == '}':
            brace_level -= 1
        
        if brace_level == 0:
            return i
    return -1

def _apply_memory_leak_fix_to_content(content: str, file_path: str) -> str:
    """
    Parses C++ content to find RNBO patcher classes and injects memory freeing code
    into destructors that are empty (whitespace-only). Destructors with any existing
    statements are left unchanged.
    """
    modified_content = content
    class_name_found = None
    
    # This regex now finds the main class which is not a subpatcher
    class_regex = re.compile(r"class\s+([a-zA-Z0-9_]+)\s*:\s*public PatcherInterfaceImpl\s*{", re.DOTALL)
    
    matches = list(class_regex.finditer(modified_content))
    
    # We assume the main patcher class is the last one in the file that matches.
    if not matches:
        return content

    # Iterate backwards to not mess up indices of later matches
    for match in reversed(matches):
        class_name = match.group(1)
        class_name_found = class_name
        
        class_body_start_match = match.end() - 1
        class_body_end = _find_closing_brace(modified_content, class_body_start_match)
        
        if class_body_end == -1:
            continue
            
        class_body_text = modified_content[class_body_start_match + 1 : class_body_end]

        # First, remove all method bodies to isolate member variables.
        method_regex = re.compile(r"\w+\s*\(.*?\)\s*\{.*?\}", re.DOTALL)
        members_only_text = re.sub(method_regex, '', class_body_text)
        
        # Also remove nested class definitions from our search space temporarily
        nested_class_regex = re.compile(r"class\s+\w+\s*:\s*public PatcherInterfaceImpl\s*\{.*?\};", re.DOTALL)
        members_only_text = re.sub(nested_class_regex, '', members_only_text)

        members_to_free = []
        for line in members_only_text.splitlines():
            stripped_line = line.strip()
            if not stripped_line or stripped_line.startswith('//') or stripped_line.startswith('/*'):
                continue

            # 1. Pointer arrays (e.g., SampleValue* signals[6];)
            m = re.match(r"^(?:SampleValue\s*\*\s*|signal\s+)([a-zA-Z0-9_]+)\[(\d+)\];", stripped_line)
            if m:
                members_to_free.append({'type': 'signal_array', 'name': m.group(1), 'size': int(m.group(2))})
                continue

            # 2. BufferRef types (e.g., Float32BufferRef my_buffer;)
            m = re.match(r"^[a-zA-Z0-9]+BufferRef\s+([a-zA-Z0-9_]+);", stripped_line)
            if m:
                members_to_free.append({'type': 'buffer_ref', 'name': m.group(1)})
                continue

            # 3. signal type (which is a pointer that needs freeing)
            m = re.match(r"^signal\s+([a-zA-Z0-9_]+);", stripped_line)
            if m:
                members_to_free.append({'type': 'signal', 'name': m.group(1)})
                continue

        if not members_to_free:
            continue

        cleanup_lines = []
        for member in members_to_free:
            if member['type'] == 'signal_array':
                for i in range(member['size']):
                    cleanup_lines.append(f"if (this->{member['name']}[{i}]) Platform::get()->free(this->{member['name']}[{i}]);")
                    cleanup_lines.append(f"this->{member['name']}[{i}] = nullptr;")
            elif member['type'] == 'signal':
                cleanup_lines.append(f"if (this->{member['name']}) Platform::get()->free(this->{member['name']});")
                cleanup_lines.append(f"this->{member['name']} = nullptr;")
            elif member['type'] == 'buffer_ref':
                cleanup_lines.append(f"this->{member['name']}.freeView();")

        if not cleanup_lines:
            continue

        # Now, find the destructor in the original class body to inject the code
        destructor_regex = re.compile(r"~\s*" + re.escape(class_name) + r"\s*\(\s*\)\s*\{")
        destructor_match = destructor_regex.search(modified_content, match.start(), class_body_end)
        
        if not destructor_match:
            continue
            
        destructor_body_start = destructor_match.end() - 1
        
        # Check if the fix is already present
        destructor_body_end = _find_closing_brace(modified_content, destructor_body_start)
        if destructor_body_end == -1:
            continue

        destructor_body_content = modified_content[destructor_body_start : destructor_body_end]
        # RNBO exports now ship with deallocateSignals() and similar in ~T(); only patch
        # legacy empty destructors. Non-empty body is left unchanged (avoids duplicate
        # teardown and bad Platform::get()->free on newer SDKs).
        if destructor_body_content.strip():
            print(
                f"Skipping ~{class_name}(): destructor not empty ({os.path.basename(file_path)})"
            )
            continue

        # Inject code before the final brace of the destructor
        insertion_point = destructor_body_end
        
        cleanup_code_str = "    " + "\n    ".join(cleanup_lines) + "\n"
        modified_content = modified_content[:insertion_point] + cleanup_code_str + modified_content[insertion_point:]

        print(f"Applied fix to ~{class_name}() in {os.path.basename(file_path)}")

    return modified_content

def main():
    if len(sys.argv) < 2:
        print("Usage: python leak_fixer.py <path_to_cpp_file>")
        sys.exit(1)

    file_path = sys.argv[1]
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        sys.exit(1)

    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()

        modified_content = _apply_memory_leak_fix_to_content(content, file_path)

        if content != modified_content:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(modified_content)
        else:
            print(f"No changes needed for {os.path.basename(file_path)}")

    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()

