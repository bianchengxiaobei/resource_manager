import type { components } from "./schema";

export type User = components["schemas"]["User"];
export type Package = components["schemas"]["Package"];
export type Version = components["schemas"]["Version"];
export type Artifact = components["schemas"]["Artifact"];
export type Channel = components["schemas"]["Channel"];
export type GrayRule = components["schemas"]["GrayRule"];
export type GrayRuleRequest = components["schemas"]["GrayRuleRequest"];
export type AuditEntry = components["schemas"]["AuditEntry"];
export type AuditPage = components["schemas"]["AuditPage"];
export type Overview = components["schemas"]["Overview"];
export type ClientConfig = components["schemas"]["ClientConfig"];

export interface MeResponse {
  user: User;
  csrf_token: string;
  api_key: boolean;
}

export interface LoginResponse {
  user: User;
  csrf_token: string;
}

let csrfToken = "";

export function setCSRF(value: string) {
  csrfToken = value;
}

export function clearCSRF() {
  csrfToken = "";
}

export class APIError extends Error {
  status: number;
  code: string;

  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers = new Headers(init.headers ?? {});
  if (init.body && !(init.body instanceof FormData) && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  if (csrfToken) {
    headers.set("X-CSRF-Token", csrfToken);
  }
  const response = await fetch(path, {
    ...init,
    headers,
    credentials: "include",
  });
  if (response.status === 204) {
    return undefined as T;
  }
  const contentType = response.headers.get("Content-Type") ?? "";
  const value = contentType.includes("application/json")
    ? await response.json()
    : await response.text();
  if (!response.ok) {
    const code = typeof value === "object" && value ? String(value.code ?? "error") : "error";
    const message =
      typeof value === "object" && value
        ? String(value.message ?? response.statusText)
        : String(value || response.statusText);
    throw new APIError(response.status, code, message);
  }
  return value as T;
}

export async function login(username: string, password: string): Promise<LoginResponse> {
  const value = await request<LoginResponse>("/api/v1/admin/login", {
    method: "POST",
    body: JSON.stringify({ username, password }),
  });
  setCSRF(value.csrf_token);
  return value;
}

export async function me(): Promise<MeResponse> {
  const value = await request<MeResponse>("/api/v1/admin/me");
  setCSRF(value.csrf_token);
  return value;
}

export async function logout(): Promise<void> {
  await request<void>("/api/v1/admin/logout", { method: "POST" });
  clearCSRF();
}

export function listPackages(): Promise<{ items: Package[] }> {
  return request("/api/v1/admin/packages");
}

export function createPackage(name: string): Promise<Package> {
  return request("/api/v1/admin/packages", {
    method: "POST",
    body: JSON.stringify({ name }),
  });
}

export function renamePackage(packageName: string, name: string): Promise<Package> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}`, {
    method: "PATCH",
    body: JSON.stringify({ name }),
  });
}

export function deletePackage(packageName: string): Promise<void> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}?confirm=${encodeURIComponent(packageName)}`,
    { method: "DELETE" },
  );
}

export function listVersions(packageName: string): Promise<{ items: Version[] }> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}/versions`);
}

export function renameVersion(packageName: string, version: string, newVersion: string): Promise<Version> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/versions/${encodeURIComponent(version)}`,
    { method: "PATCH", body: JSON.stringify({ version: newVersion }) },
  );
}

export function deleteVersion(packageName: string, version: string): Promise<void> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/versions/${encodeURIComponent(version)}` +
      `?confirm=${encodeURIComponent(version)}`,
    { method: "DELETE" },
  );
}

export function listChannels(packageName: string): Promise<{ items: Channel[] }> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}/channels`);
}

export function publish(packageName: string, version: string, channel = "prod"): Promise<Channel> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/versions/${encodeURIComponent(version)}/publish`,
    { method: "POST", body: JSON.stringify({ channel }) },
  );
}

export function rollback(packageName: string, channel = "prod", version = ""): Promise<Channel> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}/rollback`, {
    method: "POST",
    body: JSON.stringify({ channel, version }),
  });
}

export function setChannel(packageName: string, channel: string, version: string): Promise<Channel> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/channels/${encodeURIComponent(channel)}`,
    { method: "PUT", body: JSON.stringify({ version }) },
  );
}

export function listGrayRules(packageName: string): Promise<{ items: GrayRule[] }> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}/gray-rules`);
}

export function createGrayRule(packageName: string, value: GrayRuleRequest): Promise<GrayRule> {
  return request(`/api/v1/admin/packages/${encodeURIComponent(packageName)}/gray-rules`, {
    method: "POST",
    body: JSON.stringify(value),
  });
}

export function updateGrayRule(
  packageName: string,
  id: string,
  value: GrayRuleRequest,
): Promise<GrayRule> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/gray-rules/${encodeURIComponent(id)}`,
    { method: "PUT", body: JSON.stringify(value) },
  );
}

export function deleteGrayRule(packageName: string, id: string): Promise<void> {
  return request(
    `/api/v1/admin/packages/${encodeURIComponent(packageName)}/gray-rules/${encodeURIComponent(id)}`,
    { method: "DELETE" },
  );
}

export function listAudit(page = 1, pageSize = 50): Promise<AuditPage> {
  return request(`/api/v1/admin/audit?page=${page}&page_size=${pageSize}`);
}

export function overview(): Promise<Overview> {
  return request("/api/v1/admin/overview");
}

export function uploadVersion(
  packageName: string,
  version: string,
  manifest: File,
  bundles: File[],
  note: string,
  onProgress: (percent: number) => void,
): Promise<Version> {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    form.append("version", version);
    if (note) form.append("note", note);
    form.append("manifest", manifest, manifest.name);
    for (const bundle of bundles) {
      form.append("bundles", bundle, bundle.name);
    }
    const request = new XMLHttpRequest();
    request.open("POST", `/api/v1/admin/packages/${encodeURIComponent(packageName)}/versions`);
    request.withCredentials = true;
    if (csrfToken) request.setRequestHeader("X-CSRF-Token", csrfToken);
    request.upload.onprogress = (event) => {
      if (event.lengthComputable) onProgress(Math.round((event.loaded / event.total) * 100));
    };
    request.onerror = () => reject(new APIError(0, "network", "上传失败"));
    request.onload = () => {
      let payload: unknown = null;
      try {
        payload = JSON.parse(request.responseText);
      } catch {
        payload = request.responseText;
      }
      if (request.status >= 200 && request.status < 300) {
        resolve(payload as Version);
        return;
      }
      const code =
        payload && typeof payload === "object" ? String((payload as { code?: string }).code ?? "error") : "error";
      const message =
        payload && typeof payload === "object"
          ? String((payload as { message?: string }).message ?? "上传失败")
          : String(payload || "上传失败");
      reject(new APIError(request.status, code, message));
    };
    request.send(form);
  });
}
