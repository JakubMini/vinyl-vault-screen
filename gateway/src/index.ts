// The screen's only way into the vault. Cloudflare Access guards the vault on every hostname,
// so this public Worker forwards a short allowlist over a service binding. The vault still
// checks the device token on every request; this list only keeps everything else out.
const ALLOWED: [method: string, path: RegExp][] = [
  ["POST", /^\/api\/device\/(code|token)$/],
  ["GET", /^\/api\/records$/],
  ["GET", /^\/api\/records\/\d+$/],
  ["PATCH", /^\/api\/records\/\d+$/],
];

export default {
  async fetch(request, env): Promise<Response> {
    const url = new URL(request.url);
    if (!ALLOWED.some(([method, path]) => request.method === method && path.test(url.pathname))) {
      return Response.json({ error: "Not found" }, { status: 404 });
    }

    const headers = new Headers({ Accept: "application/json" });
    const auth = request.headers.get("Authorization");
    // Only device tokens pass. The vault's API key must not get round Access this way.
    if (auth?.startsWith("Bearer vvd_")) headers.set("Authorization", auth);
    const type = request.headers.get("Content-Type");
    if (type) headers.set("Content-Type", type);

    return env.VAULT.fetch(`https://vault${url.pathname}${url.search}`, {
      method: request.method,
      headers,
      body: request.method === "GET" ? null : request.body,
    });
  },
} satisfies ExportedHandler<Env>;
