// Vercel serverless function: GET /api/tle
// Fetches the latest Planet satellite TLEs from CelesTrak and returns them as JSON.
// The CDN caches the answer for 6 hours, so CelesTrak is asked at most a few times a day
// (CelesTrak asks users not to download the same data more often than every 2 hours).
module.exports = async (req, res) => {
  const url = 'https://celestrak.org/NORAD/elements/gp.php?GROUP=planet&FORMAT=tle';
  try {
    const r = await fetch(url, { headers: { 'User-Agent': 'OrbitSlot dashboard (FUSION 2026 student project)' } });
    if (!r.ok) throw new Error('CelesTrak answered ' + r.status);
    const text = await r.text();
    const lines = text.split(/\r?\n/).map(s => s.trimEnd()).filter(Boolean);
    const sats = [];
    for (let i = 0; i + 2 < lines.length + 1; i++) {
      const name = lines[i], l1 = lines[i + 1], l2 = lines[i + 2];
      if (l1 && l2 && l1.startsWith('1 ') && l2.startsWith('2 ') && !name.startsWith('1 ') && !name.startsWith('2 ')) {
        sats.push({ name: name.trim(), l1, l2 });
        i += 2;
      }
    }
    if (!sats.length) throw new Error('No TLEs found in the CelesTrak response');
    res.setHeader('Cache-Control', 's-maxage=21600, stale-while-revalidate=86400');
    res.status(200).json({ source: url, fetched: new Date().toISOString(), count: sats.length, sats });
  } catch (e) {
    res.setHeader('Cache-Control', 'no-store');
    res.status(502).json({ error: String(e && e.message || e) });
  }
};
