import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./e2e",
  timeout: 60_000,
  fullyParallel: false,
  workers: 1,
  reporter: "line",
  outputDir: "test-results",
  use: {
    baseURL: process.env.PARCEL_WEB_BASE_URL ?? "http://127.0.0.1:8080",
    channel: "msedge",
    headless: true,
    trace: "retain-on-failure",
  },
});
