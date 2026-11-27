import { createHash } from "node:crypto";
import { expect, test, type Page } from "@playwright/test";

function makeManifest(payloadText: string): Buffer {
  const payload = Buffer.from(payloadText, "utf8");
  const manifest = Buffer.alloc(160 + payload.length);
  manifest.write("PRCM", 0, "ascii");
  manifest.writeUInt16LE(1, 4);
  manifest.writeUInt16LE(160, 6);
  createHash("sha256").update(payload).digest().copy(manifest, 96);
  payload.copy(manifest, 160);
  return manifest;
}

function makeBundle(seed: number): Buffer {
  const bundle = Buffer.alloc(4096);
  bundle.write("PRCL", 0, "ascii");
  bundle.writeUInt16LE(1, 4);
  bundle.writeUInt16LE(128, 6);
  for (let index = 128; index < bundle.length; index += 1) {
    bundle[index] = (index * 31 + seed) % 251;
  }
  return bundle;
}

function versionRow(page: Page, version: string) {
  return page
    .locator(".el-table__row")
    .filter({ has: page.getByRole("button", { name: "回滚到此版本", exact: true }) })
    .filter({ hasText: version });
}

async function uploadVersion(page: Page, version: string) {
  await page.getByRole("button", { name: "上传版本", exact: true }).click();
  const dialog = page.locator(".el-dialog").filter({ hasText: "上传版本" });
  await expect(dialog).toBeVisible();
  await dialog.getByPlaceholder("1.0.0").fill(version);
  await dialog.locator('input[type="file"][accept=".bytes"]').setInputFiles({
    name: `${version}.bytes`,
    mimeType: "application/octet-stream",
    buffer: makeManifest(`manifest-${version}`),
  });
  await dialog.locator('input[type="file"][accept=".bundle"]').setInputFiles({
    name: `bundle-${version}.bundle`,
    mimeType: "application/octet-stream",
    buffer: makeBundle(version.length),
  });
  await dialog.getByRole("button", { name: "上传", exact: true }).click();
  await expect(dialog).toBeHidden({ timeout: 15_000 });
  await expect(versionRow(page, version)).toBeVisible();
}

async function publishVersion(page: Page, version: string) {
  const row = versionRow(page, version);
  await row.getByRole("button", { name: "发布", exact: true }).click();
  const confirmation = page.locator(".el-message-box");
  await expect(confirmation).toBeVisible();
  await confirmation.getByRole("button", { name: "发布", exact: true }).click();
  await expect(confirmation).toBeHidden();
  await expect(row.getByText("已发布", { exact: true })).toBeVisible();
}

async function seedGrayRules(page: Page, baseURL: string, packageName: string, count: number) {
  const origin = baseURL.replace(/\/$/, "");
  const me = await page.request.get(`${origin}/api/v1/admin/me`);
  expect(me.ok()).toBeTruthy();
  const { csrf_token: csrf } = (await me.json()) as { csrf_token: string };
  for (let index = 0; index < count; index += 1) {
    const response = await page.request.post(
      `${origin}/api/v1/admin/packages/${encodeURIComponent(packageName)}/gray-rules`,
      {
        headers: { "X-CSRF-Token": csrf },
        data: {
          version: "1.0.0",
          percentage: 1,
          whitelist: [],
          platforms: ["windows"],
          min_app_version: "",
          priority: index,
          enabled: true,
        },
      },
    );
    expect(response.status()).toBe(201);
  }
}

test("admin manages releases, gray rules, audit pagination, and metrics", async ({ page, baseURL }) => {
  const password = process.env.PARCEL_WEB_ADMIN_PASSWORD;
  if (!password) {
    throw new Error("PARCEL_WEB_ADMIN_PASSWORD is not configured");
  }
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto("/");
  await expect(page.getByText("热更分发管理端")).toBeVisible();
  await page.locator('input[autocomplete="username"]').fill("admin");
  await page.locator('input[autocomplete="current-password"]').fill(password);
  await page.getByRole("button", { name: "登录" }).click();

  await expect(page.getByRole("heading", { name: "PARCEL ADMIN" })).toBeVisible();
  await page.getByRole("button", { name: "新建包" }).click();
  const packageName = `webui-${Date.now()}`;
  await page.getByPlaceholder("game").fill(packageName);
  await page.getByRole("button", { name: "创建" }).click();
  await expect(page.locator(".package-name", { hasText: packageName })).toBeVisible();

  await uploadVersion(page, "1.0.0");
  await publishVersion(page, "1.0.0");
  await uploadVersion(page, "1.1.0");
  await publishVersion(page, "1.1.0");
  await uploadVersion(page, "1.2.0");

  const draft = versionRow(page, "1.2.0");
  await draft.getByRole("button", { name: "重命名", exact: true }).click();
  const renameVersionBox = page.locator(".el-message-box");
  await expect(renameVersionBox).toBeVisible();
  await renameVersionBox.locator("input").fill("1.2.1");
  await renameVersionBox.getByRole("button", { name: "重命名", exact: true }).click();
  await expect(versionRow(page, "1.2.1")).toBeVisible();
  await versionRow(page, "1.2.1").getByRole("button", { name: "删除", exact: true }).click();
  const deleteVersionBox = page.locator(".el-message-box");
  await expect(deleteVersionBox).toBeVisible();
  await deleteVersionBox.getByRole("button", { name: "删除", exact: true }).click();
  await expect(versionRow(page, "1.2.1")).toHaveCount(0);

  const target = versionRow(page, "1.0.0");
  await target.getByRole("button", { name: "回滚到此版本", exact: true }).click();
  const rollbackDialog = page.locator(".el-dialog").filter({ hasText: "回滚" });
  await expect(rollbackDialog).toBeVisible();
  await rollbackDialog.getByRole("button", { name: "回滚", exact: true }).click();
  await expect(rollbackDialog).toBeHidden();
  await expect(versionRow(page, "1.1.0").getByText("已回滚", { exact: true })).toBeVisible();

  const grayPanel = page
    .locator(".panel")
    .filter({ has: page.getByText("灰度规则", { exact: true }) });
  await page.getByRole("button", { name: "新增规则" }).click();
  const grayDialog = page.locator(".el-dialog").filter({ hasText: "新增灰度规则" });
  await expect(grayDialog).toBeVisible();
  await grayDialog.getByRole("button", { name: "创建", exact: true }).click();
  await expect(grayDialog).toBeHidden();
  await expect(grayPanel.getByText("10%", { exact: true })).toBeVisible();

  const grayRow = grayPanel.locator(".el-table__row").filter({ hasText: "10%" });
  await grayRow.getByRole("button", { name: "编辑", exact: true }).click();
  const editGrayDialog = page.locator(".el-dialog").filter({ hasText: "编辑灰度规则" });
  await expect(editGrayDialog).toBeVisible();
  const percentageInput = editGrayDialog
    .locator(".el-form-item")
    .filter({ hasText: "比例" })
    .locator("input");
  await percentageInput.fill("35");
  await percentageInput.blur();
  await editGrayDialog.getByRole("button", { name: "保存", exact: true }).click();
  await expect(editGrayDialog).toBeHidden();
  await expect(grayPanel.getByText("35%", { exact: true })).toBeVisible();

  const updatedGrayRow = grayPanel.locator(".el-table__row").filter({ hasText: "35%" });
  await updatedGrayRow.getByRole("button", { name: "删除", exact: true }).click();
  const deleteConfirmation = page.locator(".el-message-box");
  await expect(deleteConfirmation).toBeVisible();
  await deleteConfirmation.getByRole("button", { name: "删除", exact: true }).click();
  await expect(deleteConfirmation).toBeHidden();
  await expect(grayPanel.getByText("暂无灰度规则", { exact: true })).toBeVisible();

  const tabs = page.locator(".el-tabs__item");
  await tabs.filter({ hasText: "审计日志" }).click();
  await expect(page.getByText("gray.create", { exact: true })).toBeVisible();
  await expect(page.getByText("gray.update", { exact: true })).toBeVisible();
  await expect(page.getByText("gray.delete", { exact: true })).toBeVisible();

  await seedGrayRules(page, baseURL ?? "http://127.0.0.1:8080", packageName, 51);
  const auditPanel = page
    .locator(".panel")
    .filter({ has: page.getByText("审计日志", { exact: true }) });
  await auditPanel.getByRole("button", { name: "刷新", exact: true }).click();
  const auditPages = auditPanel.locator(".el-pager li.number");
  await expect(auditPages.filter({ hasText: "2" })).toBeVisible();
  await auditPages.filter({ hasText: "2" }).click();
  await expect(auditPanel.locator(".el-pager li.number.is-active")).toHaveText("2");
  await expect(auditPanel.locator(".el-table__row").first()).toBeVisible();

  await tabs.filter({ hasText: "指标" }).click();
  await expect(page.getByText("运行状态", { exact: true })).toBeVisible();
  await expect(page.getByText("运行秒数", { exact: true })).toBeVisible();

  await tabs.filter({ hasText: "包与版本" }).click();
  await page.getByRole("button", { name: "重命名包", exact: true }).click();
  const renamePackageBox = page.locator(".el-message-box");
  await expect(renamePackageBox).toBeVisible();
  const renamedPackage = `${packageName}-renamed`;
  await renamePackageBox.locator("input").fill(renamedPackage);
  await renamePackageBox.getByRole("button", { name: "重命名", exact: true }).click();
  await expect(page.locator(".package-name", { hasText: renamedPackage })).toBeVisible();

  await page.getByRole("button", { name: "删除包", exact: true }).click();
  const deletePackageBox = page.locator(".el-message-box");
  await expect(deletePackageBox).toBeVisible();
  await deletePackageBox.getByRole("button", { name: "删除", exact: true }).click();
  await expect(page.locator(".package-item")).toHaveCount(0);

  await page.getByRole("button", { name: "登出" }).click();
  await expect(page.getByText("热更分发管理端")).toBeVisible();
  expect(pageErrors).toEqual([]);
});
