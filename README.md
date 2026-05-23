# Harness Subagents for Cursor

这是一个面向 Cursor 的结构化多 Agent 工程模板，目标是让复杂需求按固定流程推进，而不是靠单 Agent 临场发挥。

## 目录说明

- `.cursor/agents/`：7 个子角色定义（PM、需求、方案、闸门、开发、审查、测试）
- `.cursor/rules/`：全局规则，约束流程边界与交付门槛
- `docs/workflow/pipeline.md`：阶段流转、回退与暂停规则
- `docs/contracts/*.md`：每个角色的输入/输出契约
- `docs/tasks/board.md`：任务看板（PM 每次路由都要更新）
- `dev-map.md`：开发导航图，避免重复造轮子
- `SPEC-template.md`：需求规格模板（用于新任务开场）

## 标准使用方式

1. 新建任务目录：`docs/tasks/<task-id>/`
2. 使用 `SPEC-template.md` 与用户确认需求，沉淀 `01-requirements.md`
3. 按 `pipeline.md` 串行推进：
   - `@analyst` -> `@designer` -> `@gatekeeper` -> `@developer` -> `@reviewer` -> `@tester`
4. 任何阻塞项统一用 `BLOCKING:` 开头并回退到对应上游
5. 只有 `@tester` 输出 `PASSED` 才算任务交付完成

## 最小落地约定

- PM 只路由，不给专业结论，不越权改文档
- 下游不能直接改上游产物，只能提阻塞项打回
- 开发必须做基线对比：`baseline.log` vs `post.log`
- 审查与测试都必须对齐需求 AC，不允许只看代码风格

## 建议扩展（下一步）

- 在 `.cursor/skills/` 增加编译、测试、总验证技能
- 增加 `scripts/verify-all` 统一门禁脚本
- 按项目实际情况补充 MCP 配置（CI、制品、发布、回写）

## Python 一键接入（跨项目复用）

你可以直接用这两个 Python 3 脚本把模板接入任意项目：

- `scripts/bootstrap_harness.py`：首次接入（初始化）
- `scripts/upgrade_harness.py`：后续升级（同步模板新版本）

### 1) 首次接入

```bash
python scripts/bootstrap_harness.py --target "D:/your-project" --task-id "task-001"
```

如果目标项目已有同名文件，想覆盖：

```bash
python scripts/bootstrap_harness.py --target "D:/your-project" --task-id "task-001" --force
```

### 2) 后续升级

先预览：

```bash
python scripts/upgrade_harness.py --target "D:/your-project" --dry-run
```

确认后执行：

```bash
python scripts/upgrade_harness.py --target "D:/your-project"
```

脚本会自动写入 `HARNESS_VERSION`，并把被替换内容备份到 `.harness-backup/`。

