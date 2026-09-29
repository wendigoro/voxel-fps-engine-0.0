package voxel.painter.grid;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Map-specific entities: scripted event triggers and NPCs. */
public final class MapEntities {

    /** Base class for all map entities (events, NPCs, etc.). */
    public static abstract class Entity {
        public int x, y, z;           // grid coordinates (unit voxels)
        public String id;             // unique identifier for scripting
        public String name;           // human-readable name
        public boolean enabled = true;

        protected Entity(int x, int y, int z, String id, String name) {
            this.x = x; this.y = y; this.z = z;
            this.id = id; this.name = name;
        }

        public abstract String typeName();
    }

    /** Scripted event trigger point. */
    public static final class ScriptedEvent extends Entity {
        public String scriptName;         // script file to run (relative to repo/scripts/)
        public String triggerType;        // "on_enter" | "on_interact" | "on_timer" | "on_signal"
        public float radius = 1.5f;       // activation radius in world units (voxel_size)
        public int cooldownTicks = 0;     // cooldown in ticks (0 = no cooldown)
        public String requiredSignal;     // optional signal name that must be active
        public String emitSignal;         // signal to emit when triggered
        public String condition;          // optional Lua/JS condition expression
        public int repeatLimit = -1;      // -1 = infinite, 0 = disabled, >0 = max triggers

        private int triggerCount = 0;
        private int lastTriggerTick = -1;

        public ScriptedEvent(int x, int y, int z, String id, String name, String scriptName) {
            super(x, y, z, id, name);
            this.scriptName = scriptName;
            this.triggerType = "on_enter";
        }

        @Override public String typeName() { return "scripted_event"; }

        public boolean canTrigger(int currentTick) {
            if (!enabled) return false;
            if (repeatLimit == 0) return false;
            if (repeatLimit > 0 && triggerCount >= repeatLimit) return false;
            if (cooldownTicks > 0 && lastTriggerTick >= 0 && currentTick - lastTriggerTick < cooldownTicks) return false;
            return true;
        }

        public void onTriggered(int currentTick) {
            triggerCount++;
            lastTriggerTick = currentTick;
        }

        public void reset() {
            triggerCount = 0;
            lastTriggerTick = -1;
        }
    }

    /** NPC entity with AI behavior. */
    public static final class Npc extends Entity {
        public String npcType;            // "guard" | "civilian" | "merchant" | "hostile" | "custom"
        public String aiProfile;          // AI behavior profile name
        public String patrolRouteId;      // optional patrol route reference
        public int health = 100;
        public int maxHealth = 100;
        public float moveSpeed = 0.05f;   // world units per tick
        public float viewDistance = 20.0f; // world units
        public float viewAngle = 90.0f;   // degrees
        public String faction = "neutral"; // faction for combat AI
        public String dialogueId;         // optional dialogue tree ID
        public String inventoryJson;      // optional JSON inventory
        public boolean isStatic = false;  // if true, NPC doesn't move
        public int spawnTick = 0;         // tick when NPC should spawn (0 = immediate)
        public String spawnCondition;     // optional condition for spawning

        // Runtime state (not serialized)
        public transient int currentHealth;
        public transient int currentNode = 0;
        public transient int state = 0;   // 0=idle, 1=patrol, 2=chase, 3=attack, 4=flee, 5=dead

        public Npc(int x, int y, int z, String id, String name, String npcType) {
            super(x, y, z, id, name);
            this.npcType = npcType;
            this.aiProfile = "default";
            this.currentHealth = health;
        }

        @Override public String typeName() { return "npc"; }

        public void takeDamage(int amount) {
            currentHealth = Math.max(0, currentHealth - amount);
            if (currentHealth == 0) state = 5;
        }

        public void heal(int amount) {
            currentHealth = Math.min(maxHealth, currentHealth + amount);
        }

        public boolean isAlive() { return currentHealth > 0; }
    }

    /** Patrol route waypoint. */
    public static final class PatrolNode {
        public int x, y, z;
        public float waitTime = 0.0f;     // seconds to wait at this node
        public String action;             // optional action at node ("idle", "look", "interact")

        public PatrolNode(int x, int y, int z) {
            this.x = x; this.y = y; this.z = z;
        }

        public PatrolNode(int x, int y, int z, float waitTime, String action) {
            this(x, y, z);
            this.waitTime = waitTime;
            this.action = action;
        }
    }

    /** Patrol route (list of nodes). */
    public static final class PatrolRoute {
        public String id;
        public String name;
        public boolean loop = true;
        public List<PatrolNode> nodes = new ArrayList<>();

        public PatrolRoute(String id, String name) {
            this.id = id; this.name = name;
        }

        public void addNode(PatrolNode node) { nodes.add(node); }
    }

    /** Container for all map entities. */
    public static final class MapData {
        public List<ScriptedEvent> events = new ArrayList<>();
        public List<Npc> npcs = new ArrayList<>();
        public List<PatrolRoute> patrolRoutes = new ArrayList<>();

        public void addEvent(ScriptedEvent e) { events.add(e); }
        public void addNpc(Npc n) { npcs.add(n); }
        public void addPatrolRoute(PatrolRoute r) { patrolRoutes.add(r); }

        public ScriptedEvent findEventById(String id) {
            for (ScriptedEvent e : events) if (e.id.equals(id)) return e;
            return null;
        }

        public Npc findNpcById(String id) {
            for (Npc n : npcs) if (n.id.equals(id)) return n;
            return null;
        }

        public PatrolRoute findPatrolRouteById(String id) {
            for (PatrolRoute r : patrolRoutes) if (r.id.equals(id)) return r;
            return null;
        }
    }

    /** Generate a unique entity ID. */
    public static String generateId(String prefix) {
        return prefix + "_" + System.currentTimeMillis() + "_" + (int)(Math.random() * 1000);
    }

    /** Validate entity position is within grid bounds. */
    public static boolean isValidPosition(VoxelGrid grid, int x, int y, int z) {
        return grid.inBounds(x, y, z);
    }
}