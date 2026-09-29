// Re-optimizing an optimized EPUB must keep KOSync positions pointing at the original book.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/pages/files.js'), 'utf8');
const slice = (start, end) => source.slice(source.indexOf(start), source.indexOf(end));
vm.runInThisContext(slice('const SECTION_SPLIT_SUFFIX_RE', 'const XHTML_NS'));
vm.runInThisContext(slice('// --- source-spine-map', '// --- end source-spine-map ---'));

const hrefs = ['t.html', 'c.html', 'c__ci_section_002.html'];
const locations = (extra) =>
  JSON.stringify({ spine: hrefs.map((href, index) => ({ index, href })), ...extra });
const firstPass = {
  version: 1,
  spineCount: 2,
  spine: [
    { index: 0, sourceSpineIndex: 0 },
    { index: 1, sourceSpineIndex: 1, containerDepth: 0, childRanges: [{ name: 'p', offset: 0, count: 10 }] },
    { index: 2, sourceSpineIndex: 1, containerDepth: 0, childRanges: [{ name: 'p', offset: 10, count: 8 }, { name: 'h3', offset: 0, count: 1 }] },
  ],
};
const identity = (list) => ({
  version: 1,
  spineCount: list.length,
  sourceByHref: Object.fromEntries(list.map((href, i) => [href, { sourceSpineIndex: i }])),
});

// A book that was never optimized, or optimized without moving anything, is its own original.
assert.equal(readPriorSourceSpineMap(null, hrefs).kind, 'original');
assert.equal(readPriorSourceSpineMap(JSON.stringify({ spine: [{ index: 0, href: 'a.html' }] }), ['a.html']).kind, 'original');

// A split book without its map (the double-optimization damage) cannot be repaired.
assert.equal(readPriorSourceSpineMap(locations({ chapterGroups: [{}] }), hrefs).kind, 'lost');
assert.equal(readPriorSourceSpineMap(locations({}), hrefs).kind, 'lost');
assert.equal(readPriorSourceSpineMap('{', hrefs).kind, 'lost');
// A spine edited after optimization no longer matches its map.
assert.equal(readPriorSourceSpineMap(locations({ sourceSpineMap: firstPass }), ['t.html', 'c.html']).kind, 'lost');

const prior = readPriorSourceSpineMap(locations({ sourceSpineMap: firstPass }), hrefs);
assert.equal(prior.kind, 'mapped');
assert.equal(prior.spineCount, 2);

// Nothing re-split: the first optimization's map comes back unchanged.
const unchanged = composeSourceSpineMap(prior, identity(hrefs));
assert.deepEqual(unchanged.spineCount, 2);
assert.deepEqual(unchanged.sourceByHref['t.html'], { sourceSpineIndex: 0 });
assert.deepEqual(unchanged.sourceByHref['c__ci_section_002.html'], {
  sourceSpineIndex: 1, containerDepth: 0, childRanges: [{ name: 'p', offset: 10, count: 8 }, { name: 'h3', offset: 0, count: 1 }],
});

// A part split again: its pieces are offsets inside the part's range of the original chapter.
const resplit = identity(hrefs);
resplit.sourceByHref['c__ci_section_002.html'] = { sourceSpineIndex: 2, containerDepth: 0, childRanges: [{ name: 'p', offset: 0, count: 5 }] };
resplit.sourceByHref['c__ci_section_002__ci_section_002.html'] = { sourceSpineIndex: 2, containerDepth: 0, childRanges: [{ name: 'p', offset: 5, count: 3 }, { name: 'h3', offset: 0, count: 1 }] };
const chained = composeSourceSpineMap(prior, resplit);
assert.deepEqual(chained.sourceByHref['c__ci_section_002.html'].childRanges, [{ name: 'p', offset: 10, count: 5 }]);
assert.deepEqual(chained.sourceByHref['c__ci_section_002__ci_section_002.html'].childRanges, [
  { name: 'p', offset: 15, count: 3 }, { name: 'h3', offset: 0, count: 1 },
]);
assert.equal(chained.sourceByHref['c__ci_section_002__ci_section_002.html'].sourceSpineIndex, 1);

// An unsplit input chapter split for the first time keeps its original index.
const firstSplit = identity(hrefs);
firstSplit.sourceByHref['t.html'] = { sourceSpineIndex: 0, containerDepth: 1, childRanges: [{ name: 'div', offset: 0, count: 2 }] };
assert.deepEqual(composeSourceSpineMap(prior, firstSplit).sourceByHref['t.html'], {
  sourceSpineIndex: 0, containerDepth: 1, childRanges: [{ name: 'div', offset: 0, count: 2 }],
});

// Splits along different containers, or ranges past the outer part, cannot be chained.
const otherDepth = identity(hrefs);
otherDepth.sourceByHref['c.html'] = { sourceSpineIndex: 1, containerDepth: 1, childRanges: [{ name: 'p', offset: 0, count: 2 }] };
assert.equal(composeSourceSpineMap(prior, otherDepth), null);
const pastEnd = identity(hrefs);
pastEnd.sourceByHref['c.html'] = { sourceSpineIndex: 1, containerDepth: 0, childRanges: [{ name: 'p', offset: 8, count: 3 }] };
assert.equal(composeSourceSpineMap(prior, pastEnd), null);
assert.equal(composeSourceSpineMap({ kind: 'lost' }, identity(hrefs)), null);

// First optimization: the current pass's map is already relative to the original.
const fresh = identity(hrefs);
assert.equal(composeSourceSpineMap({ kind: 'original' }, fresh), fresh);
console.log('Source spine map carry-over tests passed');
