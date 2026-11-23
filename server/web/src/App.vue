<script setup lang="ts">
import { computed, onMounted, reactive, ref } from "vue";
import { ElMessage, ElMessageBox } from "element-plus";
import * as api from "./api/client";

type TagType = "success" | "warning" | "info" | "danger" | "primary";

const user = ref<api.User | null>(null);
const busy = ref(false);
const loginForm = reactive({ username: "admin", password: "" });

const activeTab = ref("packages");
const packages = ref<api.Package[]>([]);
const selectedPackage = ref("");
const versions = ref<api.Version[]>([]);
const channels = ref<api.Channel[]>([]);
const rules = ref<api.GrayRule[]>([]);
const channelSelection = reactive<Record<string, string>>({});

const auditRows = ref<api.AuditEntry[]>([]);
const auditTotal = ref(0);
const auditPage = ref(1);
const auditPageSize = 50;
const overviewData = ref<api.Overview | null>(null);

const createPackageVisible = ref(false);
const newPackageName = ref("");
const uploadVisible = ref(false);
const uploadForm = reactive({ version: "", note: "" });
const manifestFile = ref<File | null>(null);
const bundleFiles = ref<File[]>([]);
const uploadPercent = ref(0);
const uploading = ref(false);
const rollbackVisible = ref(false);
const rollbackForm = reactive({ channel: "prod", version: "" });
const grayVisible = ref(false);
const grayEditID = ref("");
const grayForm = reactive({
  version: "",
  percentage: 10,
  whitelist: "",
  platforms: "",
  minAppVersion: "",
  priority: 0,
  enabled: true,
});

const selected = computed(() => packages.value.find((item) => item.name === selectedPackage.value));
const isAdmin = computed(() => user.value?.role === "admin");
const versionOptions = computed(() => versions.value.map((item) => item.version));
const metricRows = computed(() => {
  const counters = overviewData.value?.metrics.counters ?? {};
  return Object.entries(counters)
    .map(([name, value]) => ({ name, value }))
    .sort((left, right) => left.name.localeCompare(right.name));
});

onMounted(async () => {
  try {
    const value = await api.me();
    user.value = value.user;
    await refreshPackages();
  } catch {
    api.clearCSRF();
  }
});

async function doLogin() {
  if (!loginForm.username || !loginForm.password) {
    ElMessage.warning("请输入用户名和密码");
    return;
  }
  busy.value = true;
  try {
    const value = await api.login(loginForm.username, loginForm.password);
    user.value = value.user;
    loginForm.password = "";
    await refreshPackages();
  } catch (error) {
    ElMessage.error(errorMessage(error));
  } finally {
    busy.value = false;
  }
}

async function doLogout() {
  try {
    await api.logout();
  } catch {
    api.clearCSRF();
  }
  user.value = null;
  packages.value = [];
  selectedPackage.value = "";
  versions.value = [];
  channels.value = [];
  rules.value = [];
}

async function refreshPackages() {
  const value = await api.listPackages();
  packages.value = value.items ?? [];
  if (!packages.value.some((item) => item.name === selectedPackage.value)) {
    selectedPackage.value = packages.value[0]?.name ?? "";
  }
  if (selectedPackage.value) {
    await refreshDetail();
  }
}

async function selectPackage(name: string) {
  selectedPackage.value = name;
  await refreshDetail();
}

async function refreshDetail() {
  if (!selectedPackage.value) {
    versions.value = [];
    channels.value = [];
    rules.value = [];
    return;
  }
  try {
    const [versionValue, channelValue, ruleValue] = await Promise.all([
      api.listVersions(selectedPackage.value),
      api.listChannels(selectedPackage.value),
      api.listGrayRules(selectedPackage.value),
    ]);
    versions.value = versionValue.items ?? [];
    channels.value = channelValue.items ?? [];
    rules.value = ruleValue.items ?? [];
    for (const channel of channels.value) {
      channelSelection[channel.id] = channel.version ?? "";
    }
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

async function submitPackage() {
  const name = newPackageName.value.trim();
  if (!name) return;
  busy.value = true;
  try {
    await api.createPackage(name);
    newPackageName.value = "";
    createPackageVisible.value = false;
    await refreshPackages();
    ElMessage.success("包已创建");
  } catch (error) {
    ElMessage.error(errorMessage(error));
  } finally {
    busy.value = false;
  }
}

function openUpload() {
  uploadForm.version = "";
  uploadForm.note = "";
  manifestFile.value = null;
  bundleFiles.value = [];
  uploadPercent.value = 0;
  uploadVisible.value = true;
}

function chooseManifest(event: Event) {
  const input = event.target as HTMLInputElement;
  manifestFile.value = input.files?.[0] ?? null;
}

function chooseBundles(event: Event) {
  const input = event.target as HTMLInputElement;
  bundleFiles.value = Array.from(input.files ?? []);
}

async function submitUpload() {
  if (!selectedPackage.value || !uploadForm.version.trim() || !manifestFile.value) {
    ElMessage.warning("版本号和 manifest 必填");
    return;
  }
  uploading.value = true;
  uploadPercent.value = 0;
  try {
    await api.uploadVersion(
      selectedPackage.value,
      uploadForm.version.trim(),
      manifestFile.value,
      bundleFiles.value,
      uploadForm.note,
      (percent) => (uploadPercent.value = percent),
    );
    uploadVisible.value = false;
    await refreshDetail();
    await refreshPackages();
    ElMessage.success("版本已上传");
  } catch (error) {
    ElMessage.error(errorMessage(error));
  } finally {
    uploading.value = false;
  }
}

async function publishVersion(version: api.Version) {
  try {
    await ElMessageBox.confirm(`发布 ${version.version} 到 prod？`, "发布确认", {
      confirmButtonText: "发布",
      cancelButtonText: "取消",
    });
    await api.publish(selectedPackage.value, version.version, "prod");
    await refreshDetail();
    ElMessage.success("已发布");
  } catch (error) {
    if (error !== "cancel" && error !== "close") ElMessage.error(errorMessage(error));
  }
}

function openRollback(version?: string) {
  rollbackForm.channel = "prod";
  rollbackForm.version = version ?? "";
  rollbackVisible.value = true;
}

async function submitRollback() {
  try {
    await api.rollback(selectedPackage.value, rollbackForm.channel, rollbackForm.version);
    rollbackVisible.value = false;
    await refreshDetail();
    ElMessage.success("已回滚");
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

async function saveChannel(channel: api.Channel) {
  const version = channelSelection[channel.id];
  if (!version) return;
  try {
    await api.setChannel(selectedPackage.value, channel.name, version);
    await refreshDetail();
    ElMessage.success("渠道已更新");
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

function openGray(rule?: api.GrayRule) {
  grayEditID.value = rule?.id ?? "";
  grayForm.version = rule?.version ?? versions.value[0]?.version ?? "";
  grayForm.percentage = rule?.percentage ?? 10;
  grayForm.whitelist = rule?.whitelist?.join(", ") ?? "";
  grayForm.platforms = rule?.platforms?.join(", ") ?? "windows";
  grayForm.minAppVersion = rule?.min_app_version ?? "";
  grayForm.priority = rule?.priority ?? 0;
  grayForm.enabled = rule?.enabled ?? true;
  grayVisible.value = true;
}

async function submitGray() {
  if (!grayForm.version) {
    ElMessage.warning("请选择版本");
    return;
  }
  try {
    const payload = {
      version: grayForm.version,
      percentage: grayForm.percentage,
      whitelist: splitCSV(grayForm.whitelist),
      platforms: splitCSV(grayForm.platforms),
      min_app_version: grayForm.minAppVersion,
      priority: grayForm.priority,
      enabled: grayForm.enabled,
    };
    if (grayEditID.value) {
      await api.updateGrayRule(selectedPackage.value, grayEditID.value, payload);
    } else {
      await api.createGrayRule(selectedPackage.value, payload);
    }
    grayVisible.value = false;
    await refreshDetail();
    ElMessage.success(grayEditID.value ? "灰度规则已更新" : "灰度规则已创建");
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

async function removeGray(rule: api.GrayRule) {
  try {
    await ElMessageBox.confirm(`删除 ${rule.version} 的灰度规则？`, "删除确认", {
      confirmButtonText: "删除",
      cancelButtonText: "取消",
    });
    await api.deleteGrayRule(selectedPackage.value, rule.id);
    await refreshDetail();
  } catch (error) {
    if (error !== "cancel" && error !== "close") ElMessage.error(errorMessage(error));
  }
}

async function loadAudit(page = auditPage.value) {
  try {
    const value = await api.listAudit(page, auditPageSize);
    auditRows.value = value.items ?? [];
    auditTotal.value = value.total ?? 0;
    auditPage.value = value.page ?? page;
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

async function loadOverview() {
  try {
    overviewData.value = await api.overview();
  } catch (error) {
    ElMessage.error(errorMessage(error));
  }
}

function onTabChange(name: string) {
  if (name === "audit") void loadAudit(1);
  if (name === "metrics") void loadOverview();
}

function versionTagType(state: string): TagType {
  if (state === "published") return "success";
  if (state === "rolled_back") return "warning";
  return "info";
}

function versionTagLabel(state: string) {
  if (state === "published") return "已发布";
  if (state === "rolled_back") return "已回滚";
  return "草稿";
}

function formatTime(value?: string) {
  if (!value) return "-";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
}

function splitCSV(value: string) {
  return value
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);
}

function errorMessage(error: unknown) {
  if (error instanceof Error) return error.message;
  return String(error);
}
</script>

<template>
  <div v-if="!user" class="login-shell">
    <div class="login-card">
      <div class="brand">PARCEL</div>
      <div class="muted">热更分发管理端</div>
      <el-form label-position="top" style="margin-top: 24px" @submit.prevent="doLogin">
        <el-form-item label="用户名">
          <el-input v-model="loginForm.username" autocomplete="username" />
        </el-form-item>
        <el-form-item label="密码">
          <el-input
            v-model="loginForm.password"
            type="password"
            autocomplete="current-password"
            show-password
            @keyup.enter="doLogin"
          />
        </el-form-item>
        <el-button type="primary" style="width: 100%" :loading="busy" @click="doLogin">登录</el-button>
      </el-form>
    </div>
  </div>

  <div v-else class="shell">
    <div class="topbar">
      <div>
        <h1>PARCEL ADMIN</h1>
        <div class="muted">
          {{ user.username }} · {{ user.role === "admin" ? "管理员" : "操作员" }}
        </div>
      </div>
      <div class="toolbar">
        <el-button v-if="activeTab === 'packages' && isAdmin" @click="createPackageVisible = true">
          新建包
        </el-button>
        <el-button @click="doLogout">登出</el-button>
      </div>
    </div>

    <el-tabs v-model="activeTab" @tab-change="onTabChange">
      <el-tab-pane label="包与版本" name="packages">
        <div v-if="!packages.length" class="panel empty">还没有包。点击右上角“新建包”开始。</div>
        <div v-else class="package-layout">
          <div class="panel">
            <div class="panel-header">
              <div class="panel-title">包</div>
              <span class="muted">{{ packages.length }}</span>
            </div>
            <div class="package-list">
              <div
                v-for="item in packages"
                :key="item.id"
                class="package-item"
                :class="{ active: item.name === selectedPackage }"
                @click="selectPackage(item.name)"
              >
                <div class="package-name">{{ item.name }}</div>
                <div class="muted">{{ item.version_count }} 个版本</div>
              </div>
            </div>
          </div>

          <div v-if="selected">
            <div class="panel">
              <div class="panel-header">
                <div>
                  <div class="panel-title">{{ selected.name }}</div>
                  <div class="muted">创建于 {{ formatTime(selected.created_at) }}</div>
                </div>
                <div class="row">
                  <el-button @click="openUpload">上传版本</el-button>
                  <el-button @click="openRollback()">回滚</el-button>
                </div>
              </div>
              <el-table :data="versions" empty-text="暂无版本">
                <el-table-column prop="version" label="版本" width="130" />
                <el-table-column label="状态" width="100">
                  <template #default="{ row }">
                    <el-tag :type="versionTagType(row.state)">{{ versionTagLabel(row.state) }}</el-tag>
                  </template>
                </el-table-column>
                <el-table-column label="Manifest" min-width="260">
                  <template #default="{ row }">
                    <div class="mono">{{ row.manifest_hash }}</div>
                  </template>
                </el-table-column>
                <el-table-column label="创建时间" width="180">
                  <template #default="{ row }">{{ formatTime(row.created_at) }}</template>
                </el-table-column>
                <el-table-column label="操作" width="180">
                  <template #default="{ row }">
                    <el-button link type="primary" @click="publishVersion(row)">发布</el-button>
                    <el-button link type="warning" @click="openRollback(row.version)">回滚到此版本</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>

            <div class="panel">
              <div class="panel-header"><div class="panel-title">渠道</div></div>
              <el-table :data="channels" empty-text="暂无渠道">
                <el-table-column prop="name" label="渠道" width="160" />
                <el-table-column label="当前版本" min-width="220">
                  <template #default="{ row }">
                    <el-select v-model="channelSelection[row.id]" placeholder="选择版本" style="width: 200px">
                      <el-option v-for="version in versionOptions" :key="version" :label="version" :value="version" />
                    </el-select>
                  </template>
                </el-table-column>
                <el-table-column label="更新时间" width="190">
                  <template #default="{ row }">{{ formatTime(row.updated_at) }}</template>
                </el-table-column>
                <el-table-column label="操作" width="100">
                  <template #default="{ row }">
                    <el-button link type="primary" @click="saveChannel(row)">保存</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>

            <div class="panel">
              <div class="panel-header">
                <div class="panel-title">灰度规则</div>
                <el-button @click="openGray">新增规则</el-button>
              </div>
              <el-table :data="rules" empty-text="暂无灰度规则">
                <el-table-column prop="version" label="版本" width="120" />
                <el-table-column prop="percentage" label="比例" width="90">
                  <template #default="{ row }">{{ row.percentage }}%</template>
                </el-table-column>
                <el-table-column label="平台" width="130">
                  <template #default="{ row }">{{ row.platforms?.join(", ") || "全部" }}</template>
                </el-table-column>
                <el-table-column label="白名单" min-width="160">
                  <template #default="{ row }">{{ row.whitelist?.join(", ") || "-" }}</template>
                </el-table-column>
                <el-table-column prop="priority" label="优先级" width="90" />
                <el-table-column label="状态" width="90">
                  <template #default="{ row }">
                    <el-tag :type="row.enabled ? 'success' : 'info'">{{ row.enabled ? "启用" : "停用" }}</el-tag>
                  </template>
                </el-table-column>
                <el-table-column label="操作" width="150">
                  <template #default="{ row }">
                    <el-button link type="primary" @click="openGray(row)">编辑</el-button>
                    <el-button link type="danger" @click="removeGray(row)">删除</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>
          </div>
        </div>
      </el-tab-pane>

      <el-tab-pane label="审计日志" name="audit">
        <div class="panel">
          <div class="panel-header">
            <div class="panel-title">审计日志</div>
            <el-button @click="loadAudit(1)">刷新</el-button>
          </div>
          <el-table :data="auditRows" empty-text="暂无日志">
            <el-table-column label="时间" width="190">
              <template #default="{ row }">{{ formatTime(row.created_at) }}</template>
            </el-table-column>
            <el-table-column prop="username" label="用户" width="130" />
            <el-table-column prop="action" label="动作" width="180" />
            <el-table-column prop="target" label="对象" min-width="220" />
            <el-table-column prop="request_id" label="请求" width="210" />
          </el-table>
          <el-pagination
            style="margin-top: 16px; justify-content: flex-end"
            layout="prev, pager, next, total"
            :total="auditTotal"
            :page-size="auditPageSize"
            :current-page="auditPage"
            @current-change="loadAudit"
          />
        </div>
      </el-tab-pane>

      <el-tab-pane label="指标" name="metrics">
        <div class="panel">
          <div class="panel-header">
            <div class="panel-title">运行状态</div>
            <el-button @click="loadOverview">刷新</el-button>
          </div>
          <div v-if="overviewData" class="stat-grid">
            <div class="stat"><div class="muted">包</div><div class="stat-value">{{ overviewData.counts?.packages ?? 0 }}</div></div>
            <div class="stat"><div class="muted">版本</div><div class="stat-value">{{ overviewData.counts?.versions ?? 0 }}</div></div>
            <div class="stat"><div class="muted">渠道</div><div class="stat-value">{{ overviewData.counts?.channels ?? 0 }}</div></div>
            <div class="stat"><div class="muted">灰度规则</div><div class="stat-value">{{ overviewData.counts?.rules ?? 0 }}</div></div>
            <div class="stat"><div class="muted">运行秒数</div><div class="stat-value">{{ overviewData.metrics?.uptime_seconds ?? 0 }}</div></div>
          </div>
          <el-table :data="metricRows" style="margin-top: 18px" empty-text="暂无指标">
            <el-table-column prop="name" label="指标" min-width="420" />
            <el-table-column prop="value" label="值" width="140" />
          </el-table>
        </div>
      </el-tab-pane>
    </el-tabs>

    <el-dialog v-model="createPackageVisible" title="新建包" width="420px">
      <el-form label-position="top" @submit.prevent="submitPackage">
        <el-form-item label="包名">
          <el-input v-model="newPackageName" placeholder="game" @keyup.enter="submitPackage" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createPackageVisible = false">取消</el-button>
        <el-button type="primary" :loading="busy" @click="submitPackage">创建</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="uploadVisible" title="上传版本" width="560px">
      <el-form label-position="top">
        <el-form-item label="版本">
          <el-input v-model="uploadForm.version" placeholder="1.0.0" />
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="uploadForm.note" placeholder="可选" />
        </el-form-item>
        <el-form-item label="manifest.bytes">
          <input class="file-input" type="file" accept=".bytes" @change="chooseManifest" />
          <div class="muted">{{ manifestFile?.name ?? "未选择" }}</div>
        </el-form-item>
        <el-form-item label="Bundle 文件（可多选）">
          <input class="file-input" type="file" accept=".bundle" multiple @change="chooseBundles" />
          <div class="muted">{{ bundleFiles.length }} 个文件</div>
        </el-form-item>
        <el-progress v-if="uploading || uploadPercent" :percentage="uploadPercent" />
      </el-form>
      <template #footer>
        <el-button @click="uploadVisible = false">取消</el-button>
        <el-button type="primary" :loading="uploading" @click="submitUpload">上传</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="grayVisible" :title="grayEditID ? '编辑灰度规则' : '新增灰度规则'" width="520px">
      <el-form label-position="top">
        <el-form-item label="版本">
          <el-select v-model="grayForm.version" style="width: 100%">
            <el-option v-for="version in versionOptions" :key="version" :label="version" :value="version" />
          </el-select>
        </el-form-item>
        <el-form-item label="比例 (%)">
          <el-input-number v-model="grayForm.percentage" :min="0" :max="100" />
        </el-form-item>
        <el-form-item label="平台（逗号分隔）">
          <el-input v-model="grayForm.platforms" placeholder="windows,android" />
        </el-form-item>
        <el-form-item label="白名单（逗号分隔）">
          <el-input v-model="grayForm.whitelist" placeholder="player-001,player-002" />
        </el-form-item>
        <el-form-item label="最低 App 版本">
          <el-input v-model="grayForm.minAppVersion" placeholder="1.0.0" />
        </el-form-item>
        <el-form-item label="优先级">
          <el-input-number v-model="grayForm.priority" :min="-1000" :max="1000" />
        </el-form-item>
        <el-form-item label="启用">
          <el-switch v-model="grayForm.enabled" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="grayVisible = false">取消</el-button>
        <el-button type="primary" @click="submitGray">{{ grayEditID ? "保存" : "创建" }}</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="rollbackVisible" title="回滚" width="460px">
      <el-form label-position="top">
        <el-form-item label="渠道">
          <el-input v-model="rollbackForm.channel" />
        </el-form-item>
        <el-form-item label="目标版本（留空使用上一已发布版本）">
          <el-select v-model="rollbackForm.version" clearable filterable allow-create style="width: 100%">
            <el-option v-for="version in versionOptions" :key="version" :label="version" :value="version" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="rollbackVisible = false">取消</el-button>
        <el-button type="warning" @click="submitRollback">回滚</el-button>
      </template>
    </el-dialog>
  </div>
</template>

