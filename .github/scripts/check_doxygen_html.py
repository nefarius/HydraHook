#!/usr/bin/env python3
"""Check generated HydraHook HTML for callback and SAL regressions."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(message)


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"missing {path}")
    return path.read_text(encoding="utf-8")


def one(html_dir: Path, pattern: str) -> Path:
    matches = sorted(p for p in html_dir.glob(pattern) if p.is_file())
    if len(matches) != 1:
        names = [p.name for p in matches]
        fail(f"{pattern}: expected 1 file, found {names}")
    return matches[0]


def plain(fragment: str) -> str:
    text = re.sub(r"<[^>]+>", "", fragment)
    text = text.replace("&#160;", " ").replace("&nbsp;", " ").replace("&amp;", "&")
    return re.sub(r"\s+", " ", text).strip()


def section(html: str, start_id: str, end_id: str) -> str:
    start = html.find(f'id="{start_id}"')
    if start < 0:
        fail(f"missing section {start_id}")
    end = html.find(f'id="{end_id}"', start + len(start_id))
    if end < 0:
        end = len(html)
    return html[start:end]


def rows(fragment: str) -> list[str]:
    return re.findall(r'<tr class="memitem:.*?</tr>', fragment, flags=re.S)


def cell(row: str, css: str) -> str:
    match = re.search(rf'class="{css}">(.*?)</td>', row, flags=re.S)
    if match is None:
        fail(f"missing {css} cell")
    return plain(match.group(1))


def row_named(fragment: str, name: str) -> str:
    found = []
    for row in rows(fragment):
        match = re.search(r'class="memItemRight">(.*?)</td>', row, flags=re.S)
        if match and f">{name}</a>" in match.group(1):
            found.append(row)
    if len(found) != 1:
        fail(f"{name}: expected 1 declaration row, found {len(found)}")
    return found[0]


def doc_block(html: str, heading: str) -> str:
    needle = f"{heading}</h2>"
    start = html.find(needle)
    if start < 0:
        fail(f"missing heading {heading}")
    nxt = html.find("<h2 ", start + len(needle))
    if nxt < 0:
        nxt = len(html)
    return html[start:nxt]


def require_param(block: str, direction: str, name: str) -> None:
    pattern = (
        rf'class="paramdir">\[{re.escape(direction)}\]</td>\s*'
        rf'<td class="paramname">{re.escape(name)}</td>'
    )
    if re.search(pattern, block) is None:
        fail(f"missing [{direction}] beside {name}")


def require_sal(block: str, token: str) -> None:
    if token == "_Out_":
        pattern = r"_Out_(?!opt_)"
    elif token == "_In_":
        pattern = r"_In_(?!opt_)"
    else:
        pattern = re.escape(token)
    if re.search(pattern, block) is None:
        fail(f"missing {token} in signature")


D3D11_CALLBACKS = {
    "EVT_HYDRAHOOK_D3D11_PRE_PRESENT": (
        "pSwapChain",
        "SyncInterval",
        "Flags",
        "Extension",
    ),
    "EVT_HYDRAHOOK_D3D11_POST_PRESENT": (
        "pSwapChain",
        "SyncInterval",
        "Flags",
        "Extension",
    ),
    "EVT_HYDRAHOOK_D3D11_PRE_RESIZE_TARGET": (
        "pSwapChain",
        "pNewTargetParameters",
        "Extension",
    ),
    "EVT_HYDRAHOOK_D3D11_POST_RESIZE_TARGET": (
        "pSwapChain",
        "pNewTargetParameters",
        "Extension",
    ),
    "EVT_HYDRAHOOK_D3D11_PRE_RESIZE_BUFFERS": (
        "pSwapChain",
        "BufferCount",
        "Width",
        "Height",
        "NewFormat",
        "SwapChainFlags",
        "Extension",
    ),
    "EVT_HYDRAHOOK_D3D11_POST_RESIZE_BUFFERS": (
        "pSwapChain",
        "BufferCount",
        "Width",
        "Height",
        "NewFormat",
        "SwapChainFlags",
        "Extension",
    ),
}

D3D11_FIELDS = {
    "EvtHydraHookD3D11PrePresent": "PFN_HYDRAHOOK_D3D11_PRE_PRESENT",
    "EvtHydraHookD3D11PostPresent": "PFN_HYDRAHOOK_D3D11_POST_PRESENT",
    "EvtHydraHookD3D11PreResizeTarget": "PFN_HYDRAHOOK_D3D11_PRE_RESIZE_TARGET",
    "EvtHydraHookD3D11PostResizeTarget": "PFN_HYDRAHOOK_D3D11_POST_RESIZE_TARGET",
    "EvtHydraHookD3D11PreResizeBuffers": "PFN_HYDRAHOOK_D3D11_PRE_RESIZE_BUFFERS",
    "EvtHydraHookD3D11PostResizeBuffers": "PFN_HYDRAHOOK_D3D11_POST_RESIZE_BUFFERS",
}

D3D11_HELPERS = (
    "D3D11_DEVICE_FROM_SWAPCHAIN",
    "D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN",
    "D3D11_BACKBUFFER_FROM_SWAPCHAIN",
    "HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT",
)


def check_d3d11(html_dir: Path) -> None:
    page = read(one(html_dir, "_hydra_hook_direct3_d11_8h.html"))
    source = read(one(html_dir, "_hydra_hook_direct3_d11_8h_source.html"))
    typedefs = section(page, "typedef-members", "func-members")
    functions = section(page, "func-members", "details")

    if "_Function_class_" in typedefs or "FORCEINLINE" in page:
        fail("parser left _Function_class_ or FORCEINLINE in the Direct3D 11 reference")
    if page.count("_Function_class_(") != 0:
        fail("parsed Direct3D 11 page still contains _Function_class_")
    if source.count("_Function_class_(") < 6:
        fail("linked Direct3D 11 source lost _Function_class_")
    if "Go to the source code of this file." not in page:
        fail("Direct3D 11 page is missing its source link")

    typedef_rows = rows(typedefs)
    if len(typedef_rows) != 12:
        fail(f"Direct3D 11 typedef rows: expected 12, found {len(typedef_rows)}")
    for name, params in D3D11_CALLBACKS.items():
        row = row_named(typedefs, name)
        left = cell(row, "memItemLeft")
        right = cell(row, "memItemRight")
        if not left.startswith("typedef"):
            fail(f"{name} is {left!r}, not a typedef")
        if name not in right:
            fail(f"{name} missing from its declaration")
        for param in params:
            if param not in right:
                fail(f"{name} is missing parameter {param}: {right}")
        alias = "PFN_" + name[len("EVT_") :]
        alias_row = row_named(typedefs, alias)
        alias_left = cell(alias_row, "memItemLeft")
        alias_right = cell(alias_row, "memItemRight")
        if not alias_left.startswith("typedef") or "*" not in alias_left:
            fail(f"{alias} is not a pointer typedef: {alias_left}")
        if alias_right != alias:
            fail(f"{alias} declaration is {alias_right!r}")

    function_rows = rows(functions)
    names = [cell(row, "memItemRight").split("(", 1)[0].strip() for row in function_rows]
    if names != list(D3D11_HELPERS):
        fail(f"Direct3D 11 helpers: {names}")
    for row in function_rows:
        left = cell(row, "memItemLeft")
        if left.startswith("typedef"):
            fail(f"helper categorized as a typedef: {left}")

    init = doc_block(page, "HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT()")
    require_sal(init, "_Out_")
    require_param(init, "out", "Callbacks")

    struct = read(html_dir / "struct_h_y_d_r_a_h_o_o_k___d3_d11___e_v_e_n_t___c_a_l_l_b_a_c_k_s.html")
    attribs = section(struct, "pub-attribs", "details")
    field_rows = rows(attribs)
    if len(field_rows) != 6:
        fail(f"Direct3D 11 collection fields: expected 6, found {len(field_rows)}")
    for field, pfn in D3D11_FIELDS.items():
        row = row_named(attribs, field)
        left = cell(row, "memItemLeft")
        if pfn not in left:
            fail(f"{field} type is {left!r}, expected {pfn}")
        if f"::{field}" not in plain(struct):
            fail(f"{field} is not a collection member")


def check_initializer(html_dir: Path, page_name: str, init: str) -> None:
    page = read(one(html_dir, page_name))
    block = doc_block(page, init + "()")
    require_sal(block, "_Out_")
    require_param(block, "out", "Callbacks")
    source = read(one(html_dir, page_name.replace(".html", "_source.html")))
    if "_Function_class_(" not in source:
        fail(f"{page_name} source lost _Function_class_")


def check_core(html_dir: Path) -> None:
    page = read(one(html_dir, "_hydra_hook_core_8h.html"))
    create = doc_block(page, "HydraHookEngineCreate()")
    require_sal(create, "_In_")
    require_sal(create, "_Out_opt_")
    require_param(create, "in", "HostInstance")
    require_param(create, "in", "EngineConfig")
    require_param(create, "out", "Engine")

    alloc = doc_block(page, "HydraHookEngineAllocCustomContext()")
    require_sal(alloc, "_In_")
    require_sal(alloc, "_Out_")
    require_param(alloc, "in", "Engine")
    require_param(alloc, "out", "Context")
    require_param(alloc, "in", "ContextSize")

    log = doc_block(page, "HydraHookEngineLogDebug()")
    require_sal(log, "_In_")
    require_sal(log, "_In_opt_")
    require_param(log, "in", "Format")

    config = doc_block(page, "HYDRAHOOK_ENGINE_CONFIG_INIT()")
    require_param(config, "in,out", "EngineConfig")


def check_site(html_dir: Path) -> None:
    index = read(html_dir / "index.html")
    if 'content="Doxygen 1.18.0"' not in index:
        fail("index was not generated by Doxygen 1.18.0")
    if "cdn.jsdelivr.net" in index or "mermaid-offline.js" not in index:
        fail("landing page should load the local Mermaid module and no CDN")
    if not (html_dir / "mermaid-offline.js").is_file():
        fail("missing mermaid-offline.js")
    for page in html_dir.rglob("*.html"):
        if "cdn.jsdelivr.net" in page.read_text(encoding="utf-8"):
            fail(f"{page.name} loads a remote script")
    for stylesheet in ("doxygen-awesome.css", "doxygen-awesome-sidebar-only.css"):
        if f'href="{stylesheet}"' not in index:
            fail(f"index does not load {stylesheet}")
        if not (html_dir / stylesheet).is_file():
            fail(f"missing theme file {stylesheet}")
    if '"generateTreeView": true' not in index or '"searchEngine": true' not in index:
        fail("sidebar or search is disabled")
    for label in (
        "Public API",
        "Engine internals",
        "Core lifecycle",
        "Direct3D 9",
        "Direct3D 10",
        "Direct3D 11",
        "Direct3D 12",
        "Core Audio",
        "Implementation",
        "Architecture",
    ):
        if label not in index:
            fail(f"landing page is missing {label}")
    for name in ("navtree.js", "navtreedata.js", "search/search.js"):
        if not (html_dir / name).is_file():
            fail(f"missing navigation file {name}")
    search = "\n".join(p.read_text(encoding="utf-8") for p in (html_dir / "search").glob("*.js"))
    for symbol in ("HydraHookEngineCreate", "HookActivityTracker", "HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT"):
        if symbol not in search:
            fail(f"search index is missing {symbol}")
    annotated = read(html_dir / "annotated.html")
    class_tree = read(html_dir / "annotated_dup.js")
    nav = read(html_dir / "navtreedata.js")
    if "HookActivityTracker" not in annotated or "HookActivityTracker" not in class_tree:
        fail("C++ class index is missing HookActivityTracker")
    if "Classes" not in nav or "annotated.html" not in nav:
        fail("sidebar is missing the C++ class index")
    tracker = read(html_dir / "struct_hook_activity_tracker.html")
    if not re.search(r'href="[^"]+_source\.html"', tracker):
        fail("HookActivityTracker has no source link")


def main() -> None:
    if len(sys.argv) != 2:
        fail(f"usage: {sys.argv[0]} HTML_DIR")
    html_dir = Path(sys.argv[1])
    if not html_dir.is_dir():
        fail(f"not a directory: {html_dir}")
    check_d3d11(html_dir)
    check_initializer(html_dir, "_hydra_hook_direct3_d9_8h.html", "HYDRAHOOK_D3D9_EVENT_CALLBACKS_INIT")
    check_initializer(html_dir, "_hydra_hook_direct3_d10_8h.html", "HYDRAHOOK_D3D10_EVENT_CALLBACKS_INIT")
    check_initializer(html_dir, "_hydra_hook_direct3_d12_8h.html", "HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT")
    check_initializer(html_dir, "_hydra_hook_core_audio_8h.html", "HYDRAHOOK_ARC_EVENT_CALLBACKS_INIT")
    check_core(html_dir)
    check_site(html_dir)
    print("Doxygen HTML checks passed.")


if __name__ == "__main__":
    main()
