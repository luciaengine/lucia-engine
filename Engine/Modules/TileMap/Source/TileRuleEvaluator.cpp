#include "TileMap/TileRuleEvaluator.h"

#include "TileMap/TileMapData.h"
#include "TileMap/TileRules.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{
namespace TileMap
{
namespace
{

// Distinct salts keep the three per-cell draws independent: a rule whose chance
// roll failed must not thereby bias which tile the next rule picks.
constexpr uint32 kChanceSalt = 0x1u;
constexpr uint32 kTilePickSalt = 0x2u;

constexpr float32 kHashDenominator = 4294967296.0f;

/**
 * The draw is a pure function of (seed, cell, rule, salt) and of nothing else —
 * that is what keeps a cell's random variant stable while the region around it
 * is repainted, which is the difference between a settled map and one that
 * shimmers under the brush.
 */
uint32 HashDraw(uint32 seed, int32 cellX, int32 cellY, uint32 ruleId, uint32 salt)
{
    uint64 z = static_cast<uint64>(seed) * 0x9E3779B97F4A7C15ull;
    z ^= static_cast<uint64>(static_cast<uint32>(cellX)) * 0xBF58476D1CE4E5B9ull;
    z ^= static_cast<uint64>(static_cast<uint32>(cellY)) * 0x94D049BB133111EBull;
    z ^= static_cast<uint64>(ruleId) * 0xD6E8FEB86659FD93ull;
    z ^= static_cast<uint64>(salt) * 0xA24BAED4963EE407ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<uint32>(z >> 32);
}

float32 UnitDraw(uint32 seed, int32 cellX, int32 cellY, uint32 ruleId, uint32 salt)
{
    return static_cast<float32>(HashDraw(seed, cellX, cellY, ruleId, salt)) / kHashDenominator;
}

/** Floor-mod: the phase of a modulo rule must not flip sign across the origin. */
int64 FloorMod(int64 value, int32 divisor)
{
    const int64 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

// A map has an int32 cell domain. Reading beyond it is empty; writes and
// evaluation halos outside it are discarded rather than wrapped or clamped.
bool CellInRange(int64 x, int64 y)
{
    return x >= std::numeric_limits<int32>::min() && x <= std::numeric_limits<int32>::max() &&
           y >= std::numeric_limits<int32>::min() && y <= std::numeric_limits<int32>::max();
}

struct RuleTransform
{
    bool FlipX = false;
    bool FlipY = false;
};

TileCell ApplyTransform(TileCell cell, RuleTransform transform)
{
    if (CellIsEmpty(cell))
        return kEmptyCell;
    if (transform.FlipX)
        cell ^= kCellFlipHBit;
    if (transform.FlipY)
        cell ^= kCellFlipVBit;
    return cell;
}

bool OpAccepts(const RulePatternCell& patternCell, uint8 value)
{
    switch (patternCell.Op)
    {
    case RulePatternOp::DontCare:
        return true;
    case RulePatternOp::RequireValue:
        return value == patternCell.Value;
    case RulePatternOp::ForbidValue:
        return value != patternCell.Value;
    case RulePatternOp::RequireAny:
        return value != 0;
    }
    return true;
}

/**
 * @brief One evaluation pass over a chosen set of cells.
 *
 * Reads the source IntGrid layer, writes into a desired-cell table, and only
 * afterwards touches the map — so the dressing of a cell can never depend on the
 * dressing of its neighbour. A cell's result is therefore a function of the
 * IntGrid within twice the largest pattern radius and of nothing else, which is
 * exactly what makes the incremental pass reproduce the full one.
 *
 * Two tables rather than one: a stamped cell outranks the centre result of the
 * cell it lands on, whichever order the two were evaluated in. Only overlapping
 * stamps are settled by scan order, and their reach is bounded by that same
 * radius.
 */
class RulePass
{
public:
    RulePass(const TileMapLayerData& source, const TileRuleSet& rules)
        : m_Source(source), m_Rules(rules), m_Radius(rules.MaxPatternRadius())
    {
    }

    int32 Radius() const { return m_Radius; }

    /** Evaluates `cells` in scan order, accumulating centre and stamp results. */
    void Run(const std::vector<CellCoord>& cells)
    {
        for (const CellCoord cell : cells)
            EvaluateCell(cell);
    }

    /** The value the pass wants at `cell`; empty when no rule matched. */
    TileCell Desired(CellCoord cell) const
    {
        const uint64 packed = PackCellCoord(cell);
        if (const auto stamped = m_Stamped.find(packed); stamped != m_Stamped.end())
            return stamped->second;
        const auto centre = m_Centre.find(packed);
        return centre != m_Centre.end() ? centre->second : kEmptyCell;
    }

private:
    uint8 SourceValue(int64 cellX, int64 cellY) const
    {
        return CellInRange(cellX, cellY)
            ? m_Source.GetIntGridValue(static_cast<int32>(cellX), static_cast<int32>(cellY)) : 0;
    }

    bool MatchesPattern(const TileRule& rule, CellCoord cell, RuleTransform transform) const
    {
        const int32 size = rule.PatternSize;
        const int32 radius = RulePatternRadius(size);
        for (int32 py = 0; py < size; ++py)
        {
            for (int32 px = 0; px < size; ++px)
            {
                const int32 sx = transform.FlipX ? size - 1 - px : px;
                const int32 sy = transform.FlipY ? size - 1 - py : py;
                const RulePatternCell& patternCell =
                    rule.Pattern[static_cast<size_t>(sy) * static_cast<size_t>(size) +
                                 static_cast<size_t>(sx)];
                if (patternCell.Op == RulePatternOp::DontCare)
                    continue;
                // Pattern row 0 is the topmost row and cell Y grows upward.
                if (!OpAccepts(patternCell,
                               SourceValue(static_cast<int64>(cell.X) + px - radius,
                                           static_cast<int64>(cell.Y) + radius - py)))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool PassesGates(const TileRule& rule, CellCoord cell) const
    {
        if (rule.ModuloX > 1 && FloorMod(static_cast<int64>(cell.X) - rule.OffsetX, rule.ModuloX) != 0)
            return false;
        if (rule.ModuloY > 1 && FloorMod(static_cast<int64>(cell.Y) - rule.OffsetY, rule.ModuloY) != 0)
            return false;
        if (rule.Chance <= 0.0f)
            return false;
        if (rule.Chance < 1.0f &&
            UnitDraw(m_Rules.Seed, cell.X, cell.Y, rule.Id, kChanceSalt) >= rule.Chance)
        {
            return false;
        }
        return true;
    }

    bool FindTransform(const TileRule& rule, CellCoord cell, RuleTransform& outTransform) const
    {
        const RuleTransform candidates[]{
            {false, false}, {true, false}, {false, true}, {true, true}};
        for (const RuleTransform transform : candidates)
        {
            if (transform.FlipX && !rule.MirrorX)
                continue;
            if (transform.FlipY && !rule.MirrorY)
                continue;
            if (MatchesPattern(rule, cell, transform))
            {
                outTransform = transform;
                return true;
            }
        }
        return false;
    }

    TileCell PickTile(const TileRule& rule, CellCoord cell) const
    {
        double total = 0.0;
        for (const RuleTileOutput& output : rule.Tiles)
            total += std::max(0.0f, output.Weight);

        const double pick =
            UnitDraw(m_Rules.Seed, cell.X, cell.Y, rule.Id, kTilePickSalt) * total;
        double running = 0.0;
        TileCell chosen = kEmptyCell;
        for (const RuleTileOutput& output : rule.Tiles)
        {
            chosen = output.Cell;
            running += std::max(0.0f, output.Weight);
            if (pick < running)
                return chosen;
        }
        // All-zero weights leave `running` at 0: the last entry stands in, so a
        // rule authored without weights at all still paints.
        return chosen;
    }

    void ApplyStamp(const TileRule& rule, CellCoord cell, RuleTransform transform)
    {
        const int32 size = rule.PatternSize;
        const int32 radius = RulePatternRadius(size);
        for (int32 py = 0; py < size; ++py)
        {
            for (int32 px = 0; px < size; ++px)
            {
                const int32 sx = transform.FlipX ? size - 1 - px : px;
                const int32 sy = transform.FlipY ? size - 1 - py : py;
                const TileCell stamped =
                    rule.Stamp[static_cast<size_t>(sy) * static_cast<size_t>(size) +
                               static_cast<size_t>(sx)];
                if (CellIsEmpty(stamped))
                    continue;
                const int64 x = static_cast<int64>(cell.X) + px - radius;
                const int64 y = static_cast<int64>(cell.Y) + radius - py;
                if (!CellInRange(x, y))
                    continue;
                const CellCoord target{static_cast<int32>(x), static_cast<int32>(y)};
                m_Stamped[PackCellCoord(target)] = ApplyTransform(stamped, transform);
            }
        }
    }

    void EvaluateCell(CellCoord cell)
    {
        for (const TileRuleGroup& group : m_Rules.Groups)
        {
            if (!group.Enabled)
                continue;
            for (const TileRule& rule : group.Rules)
            {
                if (!rule.Enabled || !rule.IsWellFormed())
                    continue;
                if (!PassesGates(rule, cell))
                    continue;
                RuleTransform transform;
                if (!FindTransform(rule, cell, transform))
                    continue;

                if (rule.OutputMode == RuleOutputMode::Stamp)
                    ApplyStamp(rule, cell, transform);
                else
                    m_Centre[PackCellCoord(cell)] = ApplyTransform(PickTile(rule, cell), transform);

                // LDtk cascade: a breaking match ends this cell outright, a
                // non-breaking one lets a later group paint over the result.
                if (rule.BreakOnMatch)
                    return;
            }
        }
    }

    const TileMapLayerData& m_Source;
    const TileRuleSet& m_Rules;
    int32 m_Radius = 0;
    std::unordered_map<uint64, TileCell> m_Centre;
    std::unordered_map<uint64, TileCell> m_Stamped;
};

void SortScanOrder(std::vector<CellCoord>& cells)
{
    std::sort(cells.begin(), cells.end(), [](CellCoord a, CellCoord b)
    {
        return a.Y != b.Y ? a.Y < b.Y : a.X < b.X;
    });
}

std::vector<CellCoord> DilateCells(const std::vector<CellCoord>& cells, int32 radius)
{
    std::unordered_set<uint64> unique;
    unique.reserve(cells.size() * static_cast<size_t>((2 * radius + 1) * (2 * radius + 1)));
    for (const CellCoord cell : cells)
    {
        for (int32 dy = -radius; dy <= radius; ++dy)
        {
            for (int32 dx = -radius; dx <= radius; ++dx)
            {
                const int64 x = static_cast<int64>(cell.X) + dx;
                const int64 y = static_cast<int64>(cell.Y) + dy;
                if (CellInRange(x, y))
                    unique.insert(PackCellCoord(CellCoord{static_cast<int32>(x), static_cast<int32>(y)}));
            }
        }
    }

    std::vector<CellCoord> out;
    out.reserve(unique.size());
    for (const uint64 packed : unique)
        out.push_back(UnpackCellCoord(packed));
    SortScanOrder(out);
    return out;
}

/** Every cell of the chunks that the full pass has to look at. */
std::vector<CellCoord> FullPassRegion(const TileMapLayerData& source,
                                      const TileMapLayerData& target)
{
    std::unordered_set<uint64> chunks;
    // One chunk of dilation: the largest pattern radius is far smaller than a
    // chunk, so a ring of neighbours covers every pattern that reaches in.
    for (const ChunkCoord coord : source.SortedIntGridChunkCoords())
    {
        for (int32 dy = -1; dy <= 1; ++dy)
        {
            for (int32 dx = -1; dx <= 1; ++dx)
            {
                const int64 x = static_cast<int64>(coord.X) + dx;
                const int64 y = static_cast<int64>(coord.Y) + dy;
                if (CellInRange(x * kChunkSize, y * kChunkSize))
                    chunks.insert(PackChunkCoord(ChunkCoord{static_cast<int32>(x), static_cast<int32>(y)}));
            }
        }
    }
    // Chunks already holding auto cells, so art left without IntGrid support is
    // cleared instead of stranded.
    for (const ChunkCoord coord : target.SortedAutoChunkCoords())
        chunks.insert(PackChunkCoord(coord));

    std::vector<CellCoord> cells;
    cells.reserve(chunks.size() * static_cast<size_t>(kChunkCellCount));
    for (const uint64 packed : chunks)
    {
        const ChunkCoord chunk = UnpackChunkCoord(packed);
        for (int32 y = 0; y < kChunkSize; ++y)
        {
            for (int32 x = 0; x < kChunkSize; ++x)
            {
                const int64 cellX = static_cast<int64>(chunk.X) * kChunkSize + x;
                const int64 cellY = static_cast<int64>(chunk.Y) * kChunkSize + y;
                if (CellInRange(cellX, cellY))
                    cells.push_back(CellCoord{static_cast<int32>(cellX), static_cast<int32>(cellY)});
            }
        }
    }
    SortScanOrder(cells);
    return cells;
}

/**
 * Writes the pass result into the layer. Hand-painted cells — non-empty and not
 * flagged auto — are left exactly as they are.
 */
void ApplyPass(const RulePass& pass, TileMapLayerData& target,
               const std::vector<CellCoord>& writeCells, std::vector<RuleCellChange>& outChanged)
{
    for (const CellCoord cell : writeCells)
    {
        const TileCell current = target.GetCell(cell.X, cell.Y);
        const bool automatic = target.IsAutoCell(cell.X, cell.Y);
        if (!automatic && !CellIsEmpty(current))
            continue;

        const TileCell desired = pass.Desired(cell);
        const bool desiredAuto = !CellIsEmpty(desired);
        if (desired != current || desiredAuto != automatic)
        {
            target.SetCell(cell.X, cell.Y, desired);
            target.SetAutoCell(cell.X, cell.Y, desiredAuto);
            if (desired != current)
                outChanged.push_back(RuleCellChange{cell, current, desired, automatic, desiredAuto});
        }
    }
}

struct ResolvedLayers
{
    TileMapLayerData* Target = nullptr;
    const TileMapLayerData* Source = nullptr;
};

bool Resolve(TileMapData& map, uint32 rulesLayerId, ResolvedLayers& out)
{
    TileMapLayerData* target = map.FindLayer(rulesLayerId);
    if (!target || target->Type != TileMapLayerType::Tiles)
        return false;
    const TileMapLayerData* source = map.FindLayer(target->Rules.SourceLayerId);
    if (!source || source->Type != TileMapLayerType::IntGrid)
        return false;
    out.Target = target;
    out.Source = source;
    return true;
}

} // namespace

bool EvaluateRules(TileMapData& map, uint32 rulesLayerId, std::vector<RuleCellChange>& outChanged)
{
    ResolvedLayers layers;
    if (!Resolve(map, rulesLayerId, layers))
        return false;

    const std::vector<CellCoord> cells = FullPassRegion(*layers.Source, *layers.Target);
    RulePass pass(*layers.Source, layers.Target->Rules);
    pass.Run(cells);
    ApplyPass(pass, *layers.Target, cells, outChanged);
    return true;
}

bool EvaluateRulesIncremental(TileMapData& map, uint32 rulesLayerId,
                              const std::vector<CellCoord>& dirtyCells,
                              std::vector<RuleCellChange>& outChanged)
{
    ResolvedLayers layers;
    if (!Resolve(map, rulesLayerId, layers))
        return false;
    if (dirtyCells.empty())
        return true;

    RulePass pass(*layers.Source, layers.Target->Rules);
    const int32 radius = pass.Radius();
    // A cell's dressing reads the IntGrid within one radius, and a stamp from a
    // cell one radius away reads one radius further again — so an edit can move
    // a cell two radii away, and getting those cells right means evaluating the
    // stampers around them, one radius further still.
    const std::vector<CellCoord> writeCells = DilateCells(dirtyCells, 2 * radius);
    pass.Run(DilateCells(dirtyCells, 3 * radius));
    ApplyPass(pass, *layers.Target, writeCells, outChanged);
    return true;
}

} // namespace TileMap
} // namespace GameEngine
