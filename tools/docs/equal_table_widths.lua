-- equal_table_widths.lua — assign column widths to markdown tables
-- that pandoc would otherwise emit with auto-sized `l` columns.
--
-- GFM pipe tables can't carry width hints, so pandoc leaves
-- colspec.width == 0 and the resulting LaTeX
-- `\begin{longtable}{@{}lll@{}}` lets long-prose cells march off the
-- text block. With explicit widths assigned here pandoc emits
-- `p{...\linewidth}` columns instead.
--
-- Two column "kinds" are sized differently:
--
--   Narrow columns — every non-header cell is Code/Str/Space only and
--     ≤ NARROW_LEN_LIMIT chars (e.g. SETTINGS.md "Key", "Type",
--     "Default" columns). These are sized to their natural unbroken
--     width plus a small padding, and their content is wrapped in
--     \hbox{...} so the cell will visibly overflow if the calibration
--     is wrong, instead of silently truncating.
--   Prose columns — everything else (descriptions, notes, fixes).
--     Sized by sqrt(max_cell_len) so the longest prose cell doesn't
--     dominate.
--
-- Narrow columns are reserved first; the remainder of the line width
-- is split among prose columns by sqrt weighting. Tables that already
-- carry explicit widths are left untouched.

local utf8len = utf8.len or string.len

-- Booklet geometry: ~166 mm text block at A4 ≈ 471 pt. If
-- tools/docs/booklet.yaml geometry changes, update this.
local LINEWIDTH_PT = 471
-- 11 pt body — DejaVu Sans Mono advance width and Inter average.
local MONO_CHAR_PT = 6.6
local PROP_CHAR_PT = 5.0
-- ~6 pt padding (3 pt each side) so narrow cells don't kiss the
-- column rule.
local NARROW_PADDING_PT = 6
-- Heuristic threshold: cells longer than this are treated as prose
-- regardless of inline structure.
local NARROW_LEN_LIMIT = 30

local function inline_len(inline)
  return utf8len(pandoc.utils.stringify(inline)) or 0
end

local function cell_len(cell)
  local s = pandoc.utils.stringify(cell)
  return utf8len(s) or #s
end

local function is_narrow_inlines(inlines)
  for _, il in ipairs(inlines) do
    local tag = il.tag
    if tag ~= "Code" and tag ~= "Str" and tag ~= "Space"
       and tag ~= "SoftBreak" and tag ~= "Quoted" then
      return false
    end
  end
  return true
end

-- A cell is "narrow-eligible" when:
--   - it has 0 or 1 top-level block (Plain or Para)
--   - every inline in that block is Code / Str / Space / Quoted
--   - the stringified length is ≤ NARROW_LEN_LIMIT
local function is_narrow_cell(cell)
  local content = cell.contents
  if not content or #content == 0 then
    return true   -- empty cell doesn't disqualify the column
  end
  if #content > 1 then return false end
  local blk = content[1]
  if blk.tag ~= "Plain" and blk.tag ~= "Para" then
    return false
  end
  if not blk.content then return false end
  if not is_narrow_inlines(blk.content) then return false end
  return cell_len(cell) <= NARROW_LEN_LIMIT
end

local function cell_has_code(cell)
  local content = cell.contents
  if not content or #content == 0 then return false end
  for _, blk in ipairs(content) do
    if blk.content then
      for _, il in ipairs(blk.content) do
        if il.tag == "Code" then return true end
      end
    end
  end
  return false
end

local function rows_for_pass(t)
  local out = {}
  for _, body in ipairs(t.bodies or {}) do
    for _, r in ipairs(body.head or {}) do table.insert(out, r) end
    for _, r in ipairs(body.body or {}) do table.insert(out, r) end
  end
  if t.foot and t.foot.rows then
    for _, r in ipairs(t.foot.rows) do table.insert(out, r) end
  end
  return out
end

local function classify_columns(t, ncols)
  local body_rows = rows_for_pass(t)
  local is_narrow = {}
  local has_code = {}
  local maxlen = {}
  for i = 1, ncols do
    is_narrow[i] = true
    has_code[i] = false
    maxlen[i] = 1
  end
  -- header rows count for length but not for narrow eligibility
  -- (header text like "Description" is short by accident — we want
  -- to classify by data shape, not header text).
  if t.head then
    for _, row in ipairs(t.head.rows or {}) do
      for i, cell in ipairs(row.cells or {}) do
        local n = cell_len(cell)
        if n > maxlen[i] then maxlen[i] = n end
      end
    end
  end
  for _, row in ipairs(body_rows) do
    for i, cell in ipairs(row.cells or {}) do
      local n = cell_len(cell)
      if n > maxlen[i] then maxlen[i] = n end
      if not is_narrow_cell(cell) then is_narrow[i] = false end
      if cell_has_code(cell) then has_code[i] = true end
    end
  end
  return is_narrow, has_code, maxlen
end

local function wrap_narrow_cells(t, ncols, is_narrow)
  -- For each cell in a narrow column, wrap the cell content in
  -- raw-LaTeX `\hbox{ ... }` so the cell will visibly overflow if
  -- mis-sized rather than wrap or hyphenate silently.
  local function wrap_row(row)
    for i, cell in ipairs(row.cells or {}) do
      if is_narrow[i] then
        local content = cell.contents
        if content and #content >= 1 then
          local blk = content[1]
          if blk.content and #blk.content > 0 then
            table.insert(blk.content, 1,
              pandoc.RawInline("latex", "\\hbox{"))
            table.insert(blk.content,
              pandoc.RawInline("latex", "}"))
          end
        end
      end
    end
  end
  for _, body in ipairs(t.bodies or {}) do
    for _, r in ipairs(body.head or {}) do wrap_row(r) end
    for _, r in ipairs(body.body or {}) do wrap_row(r) end
  end
  if t.foot and t.foot.rows then
    for _, r in ipairs(t.foot.rows) do wrap_row(r) end
  end
  -- Header rows are kept wrapping-allowed: header labels like
  -- "What's happening" are prose-like and benefit from wrapping
  -- in narrow columns.
end

-- Long mono identifiers like `ScreenManager::LoadScreenRenderPrefs`
-- in prose-column cells overflow the column because `\texttt{}` has
-- no internal break points. We split each Code inline at separator
-- chars (::, _, ., -, /, =) into a sequence of shorter Code elements
-- joined by a zero-width breakable hspace, so LaTeX can break the
-- token there if the line gets too tight. Visual continuity is
-- preserved (`\hspace{0pt plus 0.0001pt}` is a near-zero stretch
-- that disappears on lines that don't need to break).
local CODE_SPLIT_THRESHOLD = 12
local SEPARATOR_PATTERN = "([:_/=%-%.])"

function Code(c)
  local text = c.text or ""
  if (utf8len(text) or #text) < CODE_SPLIT_THRESHOLD then
    return nil
  end
  local parts = {}
  local last = 1
  local i = 1
  while i <= #text do
    local ch = text:sub(i, i)
    if ch:match(SEPARATOR_PATTERN) then
      -- Coalesce runs of the same separator (e.g. `::`).
      local j = i
      while j < #text and text:sub(j + 1, j + 1) == ch do
        j = j + 1
      end
      table.insert(parts, text:sub(last, j))
      last = j + 1
      i = j + 1
    else
      i = i + 1
    end
  end
  if last <= #text then
    table.insert(parts, text:sub(last))
  end
  if #parts < 2 then return nil end
  local out = {}
  for idx, p in ipairs(parts) do
    if idx > 1 then
      table.insert(out, pandoc.RawInline(
        "latex", "\\hspace{0pt plus 0.0001pt}"))
    end
    table.insert(out, pandoc.Code(p))
  end
  return out
end

function Table(t)
  local cs = t.colspecs
  if not cs or #cs == 0 then return nil end

  for _, c in ipairs(cs) do
    local w = c[2]
    if w and w > 0 then return nil end
  end

  local ncols = #cs
  local is_narrow, has_code, maxlen = classify_columns(t, ncols)

  -- Narrow columns reserved first.
  local narrow_pt = {}
  local total_narrow_pt = 0
  for i = 1, ncols do
    if is_narrow[i] then
      local cw = has_code[i] and MONO_CHAR_PT or PROP_CHAR_PT
      narrow_pt[i] = maxlen[i] * cw + NARROW_PADDING_PT
      total_narrow_pt = total_narrow_pt + narrow_pt[i]
    end
  end

  -- Cap narrow consumption at 70% of linewidth so prose still has room.
  local NARROW_CAP_PT = LINEWIDTH_PT * 0.70
  if total_narrow_pt > NARROW_CAP_PT then
    local scale = NARROW_CAP_PT / total_narrow_pt
    for i, v in pairs(narrow_pt) do narrow_pt[i] = v * scale end
    total_narrow_pt = NARROW_CAP_PT
  end

  -- Prose budget: leftover linewidth, but never less than 25%.
  local prose_budget_pt =
    math.max(LINEWIDTH_PT - total_narrow_pt, LINEWIDTH_PT * 0.25)

  local prose_weights = {}
  local prose_sum = 0
  for i = 1, ncols do
    if not is_narrow[i] then
      local w = math.sqrt(maxlen[i])
      prose_weights[i] = w
      prose_sum = prose_sum + w
    end
  end

  local fractions = {}
  for i = 1, ncols do
    if narrow_pt[i] then
      fractions[i] = narrow_pt[i] / LINEWIDTH_PT
    elseif prose_sum > 0 then
      fractions[i] = (prose_weights[i] / prose_sum)
                     * (prose_budget_pt / LINEWIDTH_PT)
    else
      fractions[i] = 1 / ncols
    end
  end

  -- Renormalise so they sum to exactly 1.0 (avoid pandoc rounding
  -- artefacts producing 1.0001 widths).
  local total = 0
  for _, f in ipairs(fractions) do total = total + f end
  if total <= 0 then return nil end
  for i, f in ipairs(fractions) do fractions[i] = f / total end

  for i = 1, ncols do
    cs[i] = { cs[i][1], fractions[i] }
  end
  t.colspecs = cs

  wrap_narrow_cells(t, ncols, is_narrow)

  return t
end
