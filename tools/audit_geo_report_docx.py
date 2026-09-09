#!/usr/bin/env python3
"""Strict structural audit for the generated Target/geofence DOCX report.

The report is authored with python-docx and round-tripped through LibreOffice
before delivery. LibreOffice is allowed to express equivalent OOXML in a
slightly different way (for example ``w:start`` instead of ``w:left`` for
list indentation, or list spacing on the paragraph rather than the numbering
level). This audit resolves those equivalent forms instead of matching one
producer's serialization, while still requiring the actual effective layout
to satisfy the report's design contract.
"""

from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
WP = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
NS = {"w": W, "wp": WP}

CONTENT_WIDTH_DXA = 9360
TABLE_INDENT_DXA = 120
# LibreOffice can rebalance a grid by a few DXA when it reflows a table. A
# 12-DXA tolerance is below one hundredth of an inch and remains strict.
GEOMETRY_TOLERANCE_DXA = 12


def qn(local: str) -> str:
    return f"{{{W}}}{local}"


def attr(node: ET.Element | None, *names: str) -> str | None:
    """Read a Word attribute, accepting aliases used by LO/OOXML variants."""

    if node is None:
        return None
    for name in names:
        current = node.get(qn(name))
        if current is not None:
            return current
    return None


def value(node: ET.Element | None, name: str = "val") -> str | None:
    return attr(node, name)


def int_attr(node: ET.Element | None, *names: str) -> int | None:
    raw = attr(node, *names)
    if raw is None:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def direct_prop(parent: ET.Element | None, local: str) -> ET.Element | None:
    return None if parent is None else parent.find(qn(local))


def effective_table_prop(table: ET.Element, local: str) -> tuple[ET.Element | None, str]:
    """Resolve a table property without mistaking an old revision for current.

    Current direct properties take precedence. If a producer wraps the
    property in ``tblPrChange``, use that as a compatibility fallback only
    when no current direct property exists. The final descendant fallback is
    deliberately limited to the requested property name so it cannot confuse
    cell margins or other nested width-like elements.
    """

    tbl_pr = direct_prop(table, "tblPr")
    if tbl_pr is None:
        return None, "missing tblPr"

    current = direct_prop(tbl_pr, local)
    if current is not None:
        return current, "tblPr"

    for change in tbl_pr.findall(qn("tblPrChange")):
        changed_pr = direct_prop(change, "tblPr")
        candidate = direct_prop(changed_pr, local)
        if candidate is not None:
            return candidate, "tblPrChange"

    # Some serializers add an otherwise harmless wrapper around the property.
    nested = tbl_pr.find(f".//{qn(local)}")
    if nested is not None:
        return nested, "nested tblPr"
    return None, "missing"


def effective_container_prop(
    container: ET.Element,
    properties_local: str,
    local: str,
    change_local: str,
) -> tuple[ET.Element | None, str]:
    """Resolve a property below a row/cell property container.

    OOXML revisions have appeared both as a wrapper containing ``trPr`` /
    ``tcPr`` and as a change node whose property children are direct. Support
    both forms, while always preferring the current ``*Pr`` value.
    """

    properties = direct_prop(container, properties_local)
    current = direct_prop(properties, local)
    if current is not None:
        return current, properties_local
    if properties is not None:
        for change in properties.findall(qn(change_local)):
            changed_properties = direct_prop(change, properties_local) or change
            candidate = direct_prop(changed_properties, local)
            if candidate is not None:
                return candidate, change_local
    return None, "missing"


def style_chain(style_by_id: dict[str, ET.Element], style_id: str | None):
    """Yield a style and its basedOn ancestors, guarding against cycles."""

    seen: set[str] = set()
    current_id = style_id
    while current_id and current_id not in seen:
        seen.add(current_id)
        style = style_by_id.get(current_id)
        if style is None:
            return
        yield style
        current_id = value(direct_prop(style, "basedOn"))


def style_prop(
    style_by_id: dict[str, ET.Element],
    style_id: str | None,
    parent_local: str,
    prop_local: str,
) -> ET.Element | None:
    for style in style_chain(style_by_id, style_id):
        parent = direct_prop(style, parent_local)
        prop = direct_prop(parent, prop_local)
        if prop is not None:
            return prop
    return None


def paragraph_style_id(paragraph: ET.Element) -> str | None:
    p_pr = direct_prop(paragraph, "pPr")
    return value(direct_prop(p_pr, "pStyle"))


def paragraph_direct_prop(paragraph: ET.Element, local: str) -> ET.Element | None:
    return direct_prop(direct_prop(paragraph, "pPr"), local)


def paragraph_num_pr(
    paragraph: ET.Element, style_by_id: dict[str, ET.Element]
) -> ET.Element | None:
    direct = paragraph_direct_prop(paragraph, "numPr")
    if direct is not None:
        return direct
    return style_prop(style_by_id, paragraph_style_id(paragraph), "pPr", "numPr")


def numbering_level(
    numbering: ET.Element,
    num_id: int | None,
    ilvl: int | None,
) -> ET.Element | None:
    if num_id is None or ilvl is None:
        return None
    num = next(
        (node for node in numbering.findall(qn("num"))
         if int_attr(node, "numId") == num_id),
        None,
    )
    if num is None:
        return None
    abstract_id = int_attr(direct_prop(num, "abstractNumId"), "val")
    if abstract_id is None:
        return None
    abstract = next(
        (node for node in numbering.findall(qn("abstractNum"))
         if int_attr(node, "abstractNumId") == abstract_id),
        None,
    )
    if abstract is None:
        return None

    # A level override is more specific than the abstract level.
    override = next(
        (node for node in num.findall(qn("lvlOverride"))
         if int_attr(node, "ilvl") == ilvl),
        None,
    )
    if override is not None:
        override_level = direct_prop(override, "lvl")
        if override_level is not None:
            return override_level

    return next(
        (node for node in abstract.findall(qn("lvl"))
         if int_attr(node, "ilvl") == ilvl),
        None,
    )


def effective_list_prop(
    paragraph: ET.Element,
    style_by_id: dict[str, ET.Element],
    numbering: ET.Element,
    local: str,
) -> tuple[ET.Element | None, str]:
    """Resolve effective list indentation/spacing in producer-independent order."""

    direct = paragraph_direct_prop(paragraph, local)
    if direct is not None:
        return direct, "paragraph"

    num_pr = paragraph_num_pr(paragraph, style_by_id)
    num_id = int_attr(direct_prop(num_pr, "numId"), "val")
    ilvl = int_attr(direct_prop(num_pr, "ilvl"), "val")
    level = numbering_level(numbering, num_id, ilvl)
    level_prop = direct_prop(direct_prop(level, "pPr"), local)
    if level_prop is not None:
        return level_prop, "numbering level"

    styled = style_prop(style_by_id, paragraph_style_id(paragraph), "pPr", local)
    if styled is not None:
        return styled, "paragraph style"
    return None, "missing"


def page_and_style_audit(
    document: ET.Element,
    styles: ET.Element,
    errors: list[str],
) -> dict[str, ET.Element]:
    sects = document.findall(".//w:sectPr", NS)
    require(len(sects) == 1, f"expected one section, found {len(sects)}", errors)
    if sects:
        sect = sects[0]
        size = direct_prop(sect, "pgSz")
        margin = direct_prop(sect, "pgMar")
        require(size is not None, "missing page size", errors)
        require(margin is not None, "missing page margins", errors)
        if size is not None:
            require(attr(size, "w") == "12240", f"page width={attr(size, 'w')}", errors)
            require(attr(size, "h") == "15840", f"page height={attr(size, 'h')}", errors)
        if margin is not None:
            for side in ("top", "right", "bottom", "left"):
                require(attr(margin, side) == "1440",
                        f"margin {side}={attr(margin, side)}", errors)
            require(attr(margin, "header") in {"708", "720"},
                    f"header={attr(margin, 'header')}", errors)
            require(attr(margin, "footer") in {"708", "720"},
                    f"footer={attr(margin, 'footer')}", errors)

    style_by_id = {
        style.get(qn("styleId")): style
        for style in styles.findall("w:style", NS)
        if style.get(qn("styleId"))
    }
    for style_id in ("Normal", "Heading1", "Heading2", "Heading3"):
        require(style_id in style_by_id, f"missing style {style_id}", errors)

    normal = style_by_id.get("Normal")
    normal_size = style_prop(style_by_id, "Normal", "rPr", "sz")
    normal_spacing = style_prop(style_by_id, "Normal", "pPr", "spacing")
    if normal is not None:
        require(normal_size is not None, "Normal font size missing", errors)
        require(value(normal_size) == "22", f"Normal font size={value(normal_size)}", errors)
        require(normal_spacing is not None, "Normal spacing missing", errors)
        if normal_spacing is not None:
            require(attr(normal_spacing, "after") == "120",
                    f"Normal after={attr(normal_spacing, 'after')}", errors)
            require(attr(normal_spacing, "line") == "300",
                    f"Normal line={attr(normal_spacing, 'line')}", errors)

    expected_headings = {
        "Heading1": {"before": "360", "after": "200", "size": "32"},
        "Heading2": {"before": "280", "after": "140", "size": "26"},
        "Heading3": {"before": "200", "after": "100", "size": "24"},
    }
    for style_id, expected in expected_headings.items():
        if style_id not in style_by_id:
            continue
        spacing = style_prop(style_by_id, style_id, "pPr", "spacing")
        size = style_prop(style_by_id, style_id, "rPr", "sz")
        require(spacing is not None, f"{style_id} spacing missing", errors)
        if spacing is not None:
            require(attr(spacing, "before") == expected["before"],
                    f"{style_id} before={attr(spacing, 'before')}", errors)
            require(attr(spacing, "after") == expected["after"],
                    f"{style_id} after={attr(spacing, 'after')}", errors)
        require(value(size) == expected["size"],
                f"{style_id} size={value(size)}", errors)
    return style_by_id


def table_geometry_audit(document: ET.Element, errors: list[str]) -> int:
    tables = document.findall(".//w:tbl", NS)
    require(bool(tables), "no tables found", errors)
    for index, table in enumerate(tables, 1):
        width_node, width_source = effective_table_prop(table, "tblW")
        width = int_attr(width_node, "w", "val")
        width_type = attr(width_node, "type")

        grid = direct_prop(table, "tblGrid")
        grid_values = [] if grid is None else [
            int_attr(col, "w", "val")
            for col in grid.findall(qn("gridCol"))
        ]
        require(bool(grid_values), f"table {index} has no explicit tblGrid", errors)
        require(all(current is not None and current > 0 for current in grid_values),
                f"table {index} has invalid grid widths {grid_values}", errors)
        grid_sum = sum(current for current in grid_values if current is not None)

        # A fixed grid is itself a valid effective width fallback if an LO
        # serializer omitted tblW. An explicit non-dxa/auto tblW is never
        # silently accepted.
        if width is None and grid_values and all(current is not None for current in grid_values):
            width = grid_sum
            width_source = "tblGrid"
        require(width is not None, f"table {index} width is missing", errors)
        if width_type is not None:
            require(width_type == "dxa",
                    f"table {index} width type={width_type}", errors)
        if width is not None:
            require(abs(width - CONTENT_WIDTH_DXA) <= GEOMETRY_TOLERANCE_DXA,
                    f"table {index} width={width} (source {width_source})", errors)
        require(abs(grid_sum - CONTENT_WIDTH_DXA) <= GEOMETRY_TOLERANCE_DXA,
                f"table {index} tblGrid sum={grid_sum}", errors)

        indent_node, indent_source = effective_table_prop(table, "tblInd")
        indent = int_attr(indent_node, "w", "val")
        indent_type = attr(indent_node, "type")
        require(indent is not None,
                f"table {index} indent is missing (source {indent_source})", errors)
        if indent_type is not None:
            require(indent_type == "dxa",
                    f"table {index} indent type={indent_type}", errors)
        if indent is not None:
            require(abs(indent - TABLE_INDENT_DXA) <= GEOMETRY_TOLERANCE_DXA,
                    f"table {index} indent={indent} (source {indent_source})", errors)

        layout_node, layout_source = effective_table_prop(table, "tblLayout")
        layout = attr(layout_node, "type", "val")
        require(layout == "fixed",
                f"table {index} layout={layout} (source {layout_source})", errors)

        for row_index, row in enumerate(table.findall(qn("tr")), 1):
            cant_split, cant_source = effective_container_prop(
                row, "trPr", "cantSplit", "trPrChange")
            cant_value = attr(cant_split, "val")
            require(cant_split is not None and cant_value not in {"0", "false", "off"},
                    f"table {index} row {row_index} can split (source {cant_source})",
                    errors)

            # Validate each cell against the effective grid. This catches a
            # nominally 9360-DXA table whose cells still overflow or rely on
            # autofit. gridSpan is handled for merged cells.
            grid_position = 0
            for cell_index, cell in enumerate(row.findall(qn("tc")), 1):
                tc_pr = direct_prop(cell, "tcPr")
                span_node = direct_prop(tc_pr, "gridSpan")
                span = int_attr(span_node, "val") or 1
                tcw, tcw_source = effective_container_prop(
                    cell, "tcPr", "tcW", "tcPrChange")
                actual = int_attr(tcw, "w", "val")
                actual_type = attr(tcw, "type")
                require(actual is not None,
                        f"table {index} row {row_index} cell {cell_index} width missing",
                        errors)
                require(actual_type in {None, "dxa"},
                        f"table {index} row {row_index} cell {cell_index} type={actual_type}",
                        errors)
                expected = sum(grid_values[grid_position:grid_position + span]) if (
                    grid_position + span <= len(grid_values)
                ) else None
                if expected is None:
                    require(False,
                            f"table {index} row {row_index} cell {cell_index} grid span exceeds grid",
                            errors)
                elif actual is not None:
                    require(abs(actual - expected) <= GEOMETRY_TOLERANCE_DXA,
                            f"table {index} row {row_index} cell {cell_index} width={actual} "
                            f"expected={expected} (source {tcw_source})", errors)
                grid_position += span
            require(grid_position == len(grid_values),
                    f"table {index} row {row_index} covers {grid_position} of "
                    f"{len(grid_values)} grid columns", errors)
    return len(tables)


def image_audit(document: ET.Element, errors: list[str]) -> int:
    images = document.findall(".//wp:docPr", NS)
    require(len(images) == 3, f"expected 3 images, found {len(images)}", errors)
    for index, image in enumerate(images, 1):
        alt_text = (image.get("descr") or image.get("title") or "").strip()
        require(bool(alt_text), f"image {index} has no alt text", errors)
    return len(images)


def heading_and_list_audit(
    document: ET.Element,
    styles: dict[str, ET.Element],
    numbering: ET.Element,
    errors: list[str],
) -> tuple[list[int], int]:
    heading_levels: list[int] = []
    list_count = 0
    for paragraph in document.findall(".//w:body/w:p", NS):
        style_id = paragraph_style_id(paragraph)
        level: int | None = None
        if style_id:
            match = re.fullmatch(r"Heading([1-9])", style_id)
            if match:
                level = int(match.group(1))
            else:
                outline = style_prop(styles, style_id, "pPr", "outlineLvl")
                outline_value = int_attr(outline, "val")
                if outline_value is not None:
                    level = outline_value + 1
        if level is not None:
            heading_levels.append(level)

        num_pr = paragraph_num_pr(paragraph, styles)
        if num_pr is None:
            continue
        list_count += 1
        indent, indent_source = effective_list_prop(
            paragraph, styles, numbering, "ind")
        spacing, spacing_source = effective_list_prop(
            paragraph, styles, numbering, "spacing")
        left = attr(indent, "left", "start")
        hanging = attr(indent, "hanging")
        after = attr(spacing, "after")
        line = attr(spacing, "line")
        before = attr(spacing, "before")
        require(left == "540" and hanging in {"270", "271"},
                f"list paragraph {list_count} indent={left}/{hanging} "
                f"(source {indent_source})", errors)
        require(after == "80" and line == "300" and before in {None, "0"},
                f"list paragraph {list_count} spacing="
                f"before:{before},after:{after},line:{line} "
                f"(source {spacing_source})", errors)

    require(bool(heading_levels), "no styled headings", errors)
    for previous, current in zip(heading_levels, heading_levels[1:]):
        require(current <= previous + 1,
                f"heading hierarchy jumps H{previous} to H{current}", errors)
    require(list_count > 0, "no real numbered/bulleted list paragraphs", errors)
    return heading_levels, list_count


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPORT.docx", file=sys.stderr)
        return 2

    docx = Path(sys.argv[1])
    errors: list[str] = []
    try:
        with zipfile.ZipFile(docx) as archive:
            document = ET.fromstring(archive.read("word/document.xml"))
            styles = ET.fromstring(archive.read("word/styles.xml"))
            numbering = ET.fromstring(archive.read("word/numbering.xml"))
    except (OSError, KeyError, ET.ParseError) as exc:
        print(f"DOCX structural audit FAILED: cannot read OOXML ({exc})")
        return 1

    style_by_id = page_and_style_audit(document, styles, errors)
    table_count = table_geometry_audit(document, errors)
    image_count = image_audit(document, errors)
    heading_levels, list_count = heading_and_list_audit(
        document, style_by_id, numbering, errors)

    if errors:
        print("DOCX structural audit FAILED")
        for error in errors:
            print(f"- {error}")
        return 1

    print(
        "DOCX structural audit passed: Letter portrait, 1-inch margins, "
        f"{table_count} fixed tables, {image_count} alt-text images, "
        f"{len(heading_levels)} styled headings, {list_count} real list paragraphs."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
