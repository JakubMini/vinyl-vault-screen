import { describe, expect, it } from "vitest";
import gateway from "../src/index.ts";

// A stand-in for the vault's service binding that records what reached it.
function call(method: string, path: string, headers: Record<string, string> = {}, body?: string) {
  const seen: Request[] = [];
  const env = {
    VAULT: {
      fetch: async (input: string, init: RequestInit) => {
        // Node needs duplex for a streamed body; workerd does not.
        seen.push(new Request(input, { ...init, duplex: "half" } as RequestInit));
        return Response.json({ ok: true });
      },
    },
  } as unknown as Env;
  const request = new Request(`https://gateway.test${path}`, { method, headers, body });
  const response = gateway.fetch(request as Parameters<typeof gateway.fetch>[0], env);
  return { response, seen };
}

describe("gateway", () => {
  it.each([
    ["POST", "/api/device/code"],
    ["POST", "/api/device/token"],
    ["GET", "/api/records?limit=1000"],
    ["GET", "/api/records/42?limit=90"],
    ["PATCH", "/api/records/42"],
    ["GET", "/api/tags/04C67BA5892681"],
    ["PUT", "/api/tags/04C67BA5892681"],
  ])("forwards %s %s", async (method, path) => {
    const { response, seen } = call(method, path);
    expect((await response).status).toBe(200);
    expect(seen).toHaveLength(1);
    expect(seen[0]!.url).toBe(`https://vault${path}`);
    expect(seen[0]!.method).toBe(method);
  });

  it.each([
    ["GET", "/api/collection"],
    ["POST", "/api/sync/discogs"],
    ["DELETE", "/api/records/42"],
    ["POST", "/api/devices/approve"],
    ["GET", "/api/devices"],
    ["GET", "/api/records/42/../../sync/runs"],
    ["DELETE", "/api/tags/04C67BA5892681"],
    ["GET", "/api/tags/../devices"],
    ["GET", "/"],
  ])("refuses %s %s", async (method, path) => {
    const { response, seen } = call(method, path);
    expect((await response).status).toBe(404);
    expect(seen).toHaveLength(0);
  });

  it("passes a device token and the body", async () => {
    const { response, seen } = call(
      "PATCH",
      "/api/records/7",
      { Authorization: "Bearer vvd_abc", "Content-Type": "application/json" },
      JSON.stringify({ media_condition: "NM" }),
    );
    await response;
    expect(seen[0]!.headers.get("Authorization")).toBe("Bearer vvd_abc");
    expect(await seen[0]!.json()).toEqual({ media_condition: "NM" });
  });

  it("drops any other credential", async () => {
    const { response, seen } = call("GET", "/api/records", {
      Authorization: "Bearer the-api-key",
      Cookie: "CF_Authorization=x",
      "Cf-Access-Jwt-Assertion": "x",
    });
    await response;
    expect(seen[0]!.headers.get("Authorization")).toBeNull();
    expect(seen[0]!.headers.get("Cookie")).toBeNull();
    expect(seen[0]!.headers.get("Cf-Access-Jwt-Assertion")).toBeNull();
  });
});
