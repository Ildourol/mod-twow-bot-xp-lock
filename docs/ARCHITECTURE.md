# Technical Architecture: mod-twow-bot-xp-lock

## 1. Overview
`mod-twow-bot-xp-lock` is a specialized, non-intrusive bot and companion progression management module targeting the **Turtle WoW custom vMaNGOS** core (`tortoise-wow` client 1.18.1.7272 / 1.12.1) and **`TortoiseBots`** companion AI engine.

It allows players to lock or cap the XP gain of individual companion bots, enabling twink progression (levels 19, 29, 39, 49, 59) and frozen level groups without touching core sources or the companion bot engine.

---

## 2. Inviolable Architectural Invariants

* **The Unediting Doctrine**:
  * `tortoise-wow` is 100% read-only.
  * `TortoiseBots` is 100% read-only.
  * All state registries, locking logic, database schemas, command handlers, and addon message decoders reside strictly within `mod-twow-bot-xp-lock`.
* **Zero Continuation & Clean Initial Baseline**:
  * The module behaves as a fresh, standalone initial installation with idempotent database table auto-initialization (`bot_xp_locks`).
* **Profile A / Profile B Dual-Mode Compatibility**:
  * Compiles 100% cleanly in Standalone Core mode (Profile A) using only `tortoise-wow` core headers and headless session discovery (`session->IsHeadless()`).
  * If `TortoiseBots` is discovered at build time (Profile B), activates additional `HAVE_TORTOISEBOTS` preprocessor definitions for deep AI storage querying.

---

## 3. Subsystem Architecture

```mermaid
sequenceDiagram
    autonumber
    actor Player as Real Player (Client)
    participant Addon as Turtle_BotXpLock AddOn
    participant ServerNet as ServerScript (CanPacketReceive)
    participant LockMgr as BotXpLockMgr (Module Memory)
    participant DB as CharacterDatabase (tw_char)
    participant CoreHook as PlayerScript (OnGiveXP)
    participant CoreEngine as Player::GiveXP (Core Engine)

    Note over Player,Addon: Player targets Bot and clicks "Freeze XP"
    Addon->>ServerNet: CMSG_MESSAGECHAT (LANG_ADDON, prefix: TW_BOT_XPLOCK)
    ServerNet->>LockMgr: HandleAddonMessage("LOCK\t<botGuid>\t<level>")
    LockMgr->>DB: REPLACE INTO bot_xp_locks (guid, locked, lock_level...)
    LockMgr->>LockMgr: Update RAM Cache (m_locks[botGuid])
    LockMgr-->>Player: SendAddonMessage("STATUS\t<botGuid>\t<name>\t<lvl>\t1\t<cap>")
    Addon-->>Player: UI Updates Status Badge: XP LOCKED AT LEVEL X

    Note over CoreHook,CoreEngine: Bot engages mob in combat / completes quest
    CoreEngine->>CoreHook: OnGiveXP(bot, xpAmount, victim)
    CoreHook->>LockMgr: IsXpLocked(bot)
    LockMgr-->>CoreHook: Returns true
    CoreHook->>CoreEngine: Sets xpAmount = 0
    Note over CoreEngine: Core returns immediately (xp < 1); Zero XP granted!
```

---

## 4. Communication Protocol Specification

### A. Wire Protocol (`LANG_ADDON` / Prefix: `TW_BOT_XPLOCK`)
The client AddOn transmits messages formatted with tab-delimited tokens:

| Direction | Token Format | Description |
| :--- | :--- | :--- |
| **Client ➔ Server** | `QUERY\t<botGuid>` | Requests current lock state, level, and XP for the specified bot. |
| **Client ➔ Server** | `LOCK\t<botGuid>\t<capLevel>` | Requests to lock bot XP at `capLevel` (0 = current level). |
| **Client ➔ Server** | `UNLOCK\t<botGuid>` | Requests to unlock bot XP and resume normal progression. |
| **Client ➔ Server** | `ROSTER_QUERY` | Queries all companion bots in the player's active party. |
| **Server ➔ Client** | `STATUS\t<guid>\t<name>\t<lvl>\t<curXP>\t<nextXP>\t<isLocked>\t<lockLevel>` | Broadcasts authoritative bot progression state to recipient. |

### B. Command Protocol (`AllCommandScript`)
For solo play or direct console management, commands are routed through `.botxplock`:
* `.botxplock status`: Displays target bot's lock state.
* `.botxplock lock [level]`: Locks target bot immediately or caps at `[level]`.
* `.botxplock unlock`: Removes target bot's XP lock.
* `.botxplock list`: Outputs all currently locked bots and their active levels.

---

## 5. Concurrency & Data Safety

* **Thread-Safe RAM Cache**:
  All per-bot lock states are held in an in-memory hash map (`std::unordered_map<uint32, LockEntry>`) guarded by a `std::mutex` (`m_mutex`). Read operations during high-frequency combat ticks (`OnGiveXP`) execute in sub-microsecond time.
* **Persistent State Synchronization**:
  State changes are written to MySQL table `tw_char.bot_xp_locks`. On server boot or `.server reload config`, states are populated into RAM automatically during `WorldScript::OnStartup`.
* **Alt Level Sync Protection**:
  When `PlayerbotAI.SyncAltLevelToMaster = 1`, the bot engine attempts to level alt bots up to match the master. `BotXpLockMgr::OnLevelChanged` intercepts unintended dings exceeding the target cap and clamps the bot back to its designated bracket level.
