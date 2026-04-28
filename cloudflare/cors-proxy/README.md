# SlothDB Playground CORS Proxy

A 60-line Cloudflare Worker that lets the browser playground fetch
Parquet/CSV/JSON files from S3 buckets that don't set
`Access-Control-Allow-Origin`.

Public CORS proxies (`corsproxy.io`, `api.allorigins.win`, etc.) reliably
fail on files larger than ~10 MB or rate-limit specific hosts. This
Worker has no size limit (Cloudflare Workers stream up to ~25 MB request
and unlimited response on the free tier).

## Deploy

```bash
npm install -g wrangler
wrangler login
cd cloudflare/cors-proxy
wrangler deploy
```

Wrangler will print a URL like:

```
https://slothdb-cors-proxy.<your-subdomain>.workers.dev
```

## Wire it into the playground

Edit `docs/playground/index.html` and add a `<script>` block before
`app.js` loads:

```html
<script>
  // Use your own Worker for CORS proxy. Format must end in `?url=`.
  window.SLOTHDB_CORS_PROXY = 'https://slothdb-cors-proxy.<your-subdomain>.workers.dev/?url=';
</script>
```

Bump the cache-buster in `app.js` and `index.html` (search for
`BUILD_VERSION`) and push. The playground will now route blocked fetches
through your Worker.

## Restricting access

Open by default. To pin it to specific upstream hosts, uncomment the
`ALLOWED_HOSTS` line in `wrangler.toml` and redeploy.

## Cost

Cloudflare Workers free tier: 100k requests/day. Each playground query
that loads N files is N requests. Plenty for a public demo.
