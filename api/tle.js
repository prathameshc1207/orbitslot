// Vercel serverless function: GET /api/tle
// Fetches the latest Planet satellite TLEs from CelesTrak and returns them as JSON.
// The CDN caches the answer for 6 hours, so CelesTrak is asked at most a few times a day
// (CelesTrak asks users not to download the same data more often than every 2 hours).
const SOURCES = [
  'https://celestrak.org/NORAD/elements/gp.php?GROUP=planet&FORMAT=tle',
  'https://celestrak.org/NORAD/elements/gp.php?NAME=FLOCK&FORMAT=tle'
];
function parse(text) {
  const lines = text.split(/\r?\n/).map(s => s.trimEnd()).filter(Boolean), sats = [];
  for (let i = 0; i + 2 < lines.length + 1; i++) {
    const name = lines[i], l1 = lines[i + 1], l2 = lines[i + 2];
    if (l1 && l2 && l1.startsWith('1 ') && l2.startsWith('2 ') && !name.startsWith('1 ') && !name.startsWith('2 ')) {
      sats.push({ name: name.trim(), l1, l2 }); i += 2;
    }
  }
  return sats;
}
async function handler(req, res) {
  const errors = [];
  for (const url of SOURCES) {
    try {
      const ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), 12000);
      const r = await fetch(url, { signal: ctl.signal, headers: { 'User-Agent': 'OrbitSlot dashboard (FUSION 2026 student project)' } });
      clearTimeout(timer);
      const text = await r.text();
      if (!r.ok) { errors.push(`CelesTrak answered HTTP ${r.status}: ${text.slice(0, 120).trim()}`); continue; }
      const sats = parse(text);
      if (!sats.length) { errors.push('CelesTrak returned no TLEs: ' + text.slice(0, 120).trim()); continue; }
      res.setHeader('Cache-Control', 's-maxage=21600, stale-while-revalidate=86400');
      return res.status(200).json({ source: url, fetched: new Date().toISOString(), count: sats.length, sats });
    } catch (e) { errors.push(e.name === 'AbortError' ? 'CelesTrak took too long to answer' : String(e.message || e)); }
  }
  res.setHeader('Cache-Control', 'no-store');
  res.status(502).json({ error: errors.join(' | ') });
}
module.exports = handler;
module.exports.config = { maxDuration: 30 };
