// Vercel serverless function: GET /api/satnogs
// Pulls real observation data from the SatNOGS Network (https://network.satnogs.org),
// an open worldwide network of volunteer satellite ground stations.
// Returns observations scheduled or running in the next hours, and recent successful receptions.
// Cached for 15 minutes so we stay well inside SatNOGS' anonymous limit (60 requests per hour).
const BASE = 'https://network.satnogs.org/api/observations/?format=json';
const keep = o => ({
  id: o.id, start: o.start, end: o.end, status: o.status,
  ground_station: o.ground_station, station_name: o.station_name,
  station_lat: o.station_lat, station_lng: o.station_lng,
  norad_cat_id: o.norad_cat_id, tle0: o.tle0, tle1: o.tle1, tle2: o.tle2,
  max_altitude: o.max_altitude, frequency: o.observation_frequency,
  mode: o.transmitter_mode, description: o.transmitter_description, baud: o.transmitter_baud,
  waterfall: o.waterfall, demod_count: Array.isArray(o.demoddata) ? o.demoddata.length : 0,
  observer: o.observer
});
async function get(url, ms) {
  const ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), ms);
  try {
    const r = await fetch(url, { signal: ctl.signal, headers: { 'Accept': 'application/json', 'User-Agent': 'OrbitSlot dashboard (FUSION 2026 student project)' } });
    if (!r.ok) throw new Error('SatNOGS answered HTTP ' + r.status);
    const j = await r.json();
    return Array.isArray(j) ? j : (j.results || []);
  } finally { clearTimeout(timer); }
}
async function handler(req, res) {
  const now = Date.now(), iso = t => new Date(t).toISOString().slice(0, 19) + 'Z';
  const jobs = [
    get(`${BASE}&status=future&start=${iso(now - 30 * 60000)}&end=${iso(now + 8 * 3600000)}`, 12000),
    get(`${BASE}&status=good&end=${iso(now)}`, 12000),
    get(BASE, 12000)   // plain latest list, as a backup if the filtered queries fail
  ];
  const out = await Promise.allSettled(jobs);
  const ok = out.filter(x => x.status === 'fulfilled');
  if (!ok.length) {
    res.setHeader('Cache-Control', 'no-store');
    return res.status(502).json({ error: out.map(x => x.reason && (x.reason.name === 'AbortError' ? 'SatNOGS took too long to answer' : x.reason.message)).join('; ') });
  }
  const val = i => out[i].status === 'fulfilled' ? out[i].value : [];
  const backup = val(2), t = Date.now();
  let upcoming = val(0), recent = val(1);
  if (!upcoming.length) upcoming = backup.filter(o => Date.parse(o.end) > t);
  if (!recent.length) recent = backup.filter(o => o.status === 'good');
  res.setHeader('Cache-Control', 's-maxage=900, stale-while-revalidate=3600');
  res.status(200).json({ source: 'network.satnogs.org', fetched: new Date().toISOString(), upcoming: upcoming.map(keep), recent: recent.map(keep), partial: ok.length < jobs.length });
}
module.exports = handler;
module.exports.config = { maxDuration: 30 };
