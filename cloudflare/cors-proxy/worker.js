// Tiny CORS proxy for the SlothDB playground.
//
// Why this exists: the WASM playground runs in a browser, which means
// every fetch is subject to CORS. Most public S3 buckets and TPC-H
// datasets do NOT send Access-Control-Allow-Origin, so the playground
// can't read them directly. Public CORS proxies (corsproxy.io, allorigins)
// reliably fail on files larger than ~10 MB.
//
// This Worker fetches the URL server-side (no CORS) and streams the
// body back with Access-Control-Allow-Origin set. It's stateless,
// keeps no logs, and respects the upstream Content-Type / Range / etc.
//
// Deploy:
//   npm i -g wrangler
//   wrangler login
//   wrangler deploy
// Then point the playground at it via window.SLOTHDB_CORS_PROXY in
// docs/playground/index.html, e.g.
//   window.SLOTHDB_CORS_PROXY = 'https://cors-proxy.<your-subdomain>.workers.dev/?url=';
//
// Optional ALLOWED_HOSTS env var restricts which upstream hosts the
// Worker will fetch. Comma-separated. Empty/unset = allow any.

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const target = url.searchParams.get('url');

    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: corsHeaders() });
    }

    if (!target) {
      return new Response('Use ?url=<encoded-https-url>', {
        status: 400,
        headers: corsHeaders(),
      });
    }

    let upstream;
    try {
      upstream = new URL(target);
    } catch {
      return new Response('Bad ?url= value', { status: 400, headers: corsHeaders() });
    }
    if (upstream.protocol !== 'https:' && upstream.protocol !== 'http:') {
      return new Response('Only http(s) URLs allowed', { status: 400, headers: corsHeaders() });
    }

    const allow = (env.ALLOWED_HOSTS || '').split(',').map(s => s.trim()).filter(Boolean);
    if (allow.length && !allow.some(h => upstream.host === h || upstream.host.endsWith('.' + h))) {
      return new Response(`Host not in ALLOWED_HOSTS: ${upstream.host}`, {
        status: 403,
        headers: corsHeaders(),
      });
    }

    // Forward the user's Range header so the engine's HTTP-range path keeps working.
    const fwdHeaders = new Headers();
    const range = request.headers.get('Range');
    if (range) fwdHeaders.set('Range', range);
    fwdHeaders.set('User-Agent', 'slothdb-cors-proxy/1');

    const upstreamResp = await fetch(target, {
      method: request.method === 'HEAD' ? 'HEAD' : 'GET',
      headers: fwdHeaders,
      redirect: 'follow',
    });

    const headers = new Headers();
    headers.set('Access-Control-Allow-Origin', '*');
    headers.set('Access-Control-Expose-Headers', 'Content-Length, Content-Range, Accept-Ranges');
    for (const k of ['Content-Type', 'Content-Length', 'Content-Range', 'Accept-Ranges', 'Last-Modified', 'ETag']) {
      const v = upstreamResp.headers.get(k);
      if (v) headers.set(k, v);
    }
    headers.set('Cache-Control', 'public, max-age=300');

    return new Response(upstreamResp.body, {
      status: upstreamResp.status,
      statusText: upstreamResp.statusText,
      headers,
    });
  },
};

function corsHeaders() {
  return {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, HEAD, OPTIONS',
    'Access-Control-Allow-Headers': 'Range',
    'Access-Control-Max-Age': '86400',
  };
}
