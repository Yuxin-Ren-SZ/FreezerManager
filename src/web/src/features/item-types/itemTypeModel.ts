// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  FieldDataType,
  ScopeKind,
  type CustomFieldDefinition,
  type ItemType,
} from '../../gen/fmgr/v1/item_type_pb';

/**
 * The item-type admin's rules as values (TODO.md G3.9, PRD §4.3, N5).
 *
 * Deliberately pure and React-free: the tree editor, the per-node field list
 * and the definition form all need the same answers, and these are the parts
 * that have to be unit-testable without mounting anything — in particular the
 * two guards below, whose failure mode is a hang or a silently loosened
 * constraint rather than a visible error.
 *
 * **Where the rules come from.** `src/storage/CustomFieldResolver.h` is the
 * server's inheritance rule and `src/storage/sqlite/ItemTypeRepositories.cc`
 * its cycle guard; this module mirrors both, including their cycle handling:
 *
 *  - the resolver walks leaf → root with an `unordered_set` of visited ids and
 *    *breaks* on a repeat, so a definition is never applied twice and the walk
 *    always ends. There is no depth limit, because a legitimate taxonomy is
 *    deep and an arbitrary cap would truncate it (the mistake `layoutModel.ts`
 *    records for G3.1);
 *  - the repository's `check_no_cycle` walks the proposed parent chain and
 *    throws on a repeat. The server therefore refuses a cycle even though this
 *    screen makes one impossible to create by dragging — both halves are the
 *    acceptance criterion, not either.
 *
 * The client guard exists because a drop is a *drag*: keeping a dragged node
 * inside its own subtree is a UI mistake, and the fix is to refuse the drop,
 * not to send a request the server will reject. The server guard exists because
 * the client's tree can be stale — another admin re-parents a node between the
 * load and the drop — and because the client is not an enforcement point.
 */

const isArchived = (row: { readonly archivedAt?: unknown }): boolean =>
  row.archivedAt !== undefined;

/** Siblings in a stable order: by name, then id. Mirrors the Qt/Layout sort. */
function compareItemTypes(a: ItemType, b: ItemType): number {
  return a.name.localeCompare(b.name) || a.id.localeCompare(b.id);
}

/** One node of the taxonomy, with its subtree. */
export interface ItemTypeNode {
  readonly type: ItemType;
  /** 0 for a root. */
  readonly depth: number;
  readonly children: readonly ItemTypeNode[];
}

export interface ItemTypeTree {
  readonly roots: readonly ItemTypeNode[];
  /**
   * Rows that sit on a parent cycle (`A → B → A`). Impossible to create here,
   * so they come from outside this screen; they still render, as roots with
   * their parent edge dropped.
   */
  readonly cyclic: readonly ItemType[];
  /** Rows whose `parent_id` is not in the loaded set. Rendered as roots. */
  readonly orphaned: readonly ItemType[];
}

/**
 * The forest to render. Terminates on any input, including a parent cycle:
 * every node is visited at most once, and a node on a cycle becomes a root
 * with its parent edge dropped rather than a node no root can reach.
 */
export function buildItemTypeTree(types: readonly ItemType[]): ItemTypeTree {
  const byId = new Map(types.map((type) => [type.id, type]));
  const cyclicIds = findCyclicIds(types, byId);

  const childrenOf = new Map<string, ItemType[]>();
  const roots: ItemType[] = [];
  const orphaned: ItemType[] = [];

  for (const type of types) {
    // A cyclic node's parent edge is the edge that closes the cycle: dropping
    // it is what makes the rest of the graph a forest again.
    const parentId =
      type.parentId === undefined || cyclicIds.has(type.id) ? undefined : type.parentId;
    if (parentId === undefined) {
      roots.push(type);
      if (type.parentId !== undefined && !cyclicIds.has(type.id)) {
        orphaned.push(type);
      }
      continue;
    }
    if (!byId.has(parentId)) {
      roots.push(type);
      orphaned.push(type);
      continue;
    }
    const siblings = childrenOf.get(parentId) ?? [];
    siblings.push(type);
    childrenOf.set(parentId, siblings);
  }

  const visited = new Set<string>();
  const output: ItemTypeNode[] = [];
  const stack: { readonly type: ItemType; readonly depth: number; readonly out: ItemTypeNode[] }[] =
    [...roots]
      .sort(compareItemTypes)
      .reverse()
      .map((type) => ({ type, depth: 0, out: output }));

  while (stack.length > 0) {
    const frame = stack.pop();
    if (frame === undefined) {
      break;
    }
    // The visited set, not a depth limit: a node is rendered once however the
    // data is shaped, and a malformed edge cannot make this loop run forever.
    if (visited.has(frame.type.id)) {
      continue;
    }
    visited.add(frame.type.id);
    const children: ItemTypeNode[] = [];
    const node: ItemTypeNode = { type: frame.type, depth: frame.depth, children };
    frame.out.push(node);
    const sorted = [...(childrenOf.get(frame.type.id) ?? [])].sort(compareItemTypes);
    // Reversed so `pop()` visits them in name order.
    for (const child of sorted.reverse()) {
      stack.push({ type: child, depth: frame.depth + 1, out: children });
    }
  }

  return {
    roots: output,
    cyclic: types.filter((type) => cyclicIds.has(type.id)),
    orphaned,
  };
}

/**
 * The ids that sit on a parent cycle.
 *
 * One walk per node, each carrying its own `seen` set: when a walk revisits a
 * node it started from, everything from that node onward is on the cycle. The
 * set is what makes this terminate on the cyclic input it exists to detect.
 */
function findCyclicIds(types: readonly ItemType[], byId: ReadonlyMap<string, ItemType>) {
  const cyclic = new Set<string>();
  const known = new Map<string, boolean>();

  for (const type of types) {
    if (known.get(type.id) === false) {
      continue;
    }
    const path: string[] = [];
    const seen = new Map<string, number>();
    let cursor: string | undefined = type.id;
    while (cursor !== undefined) {
      const index = seen.get(cursor);
      if (index !== undefined) {
        for (const id of path.slice(index)) {
          cyclic.add(id);
          known.set(id, true);
        }
        break;
      }
      seen.set(cursor, path.length);
      path.push(cursor);
      cursor = byId.get(cursor)?.parentId;
    }
    for (const id of path) {
      known.set(id, known.get(id) ?? false);
    }
  }
  return cyclic;
}

/**
 * The node's id chain, leaf first, root last (the resolver's `chain`).
 *
 * Stops at a missing parent or at a repeat, so a cycle returns the nodes it
 * walked rather than looping.
 */
export function lineageOf(types: readonly ItemType[], nodeId: string): readonly string[] {
  const byId = new Map(types.map((type) => [type.id, type]));
  if (!byId.has(nodeId)) {
    return [];
  }
  const chain: string[] = [];
  const seen = new Set<string>();
  let cursor: string | undefined = nodeId;
  while (cursor !== undefined && byId.has(cursor) && !seen.has(cursor)) {
    seen.add(cursor);
    chain.push(cursor);
    cursor = byId.get(cursor)?.parentId;
  }
  return chain;
}

/** Where an effective definition is attached, relative to the open node. */
export type FieldOrigin = 'lab' | 'node' | 'ancestor';

/** One key's winning definition for a node, and what it shadows. */
export interface EffectiveField {
  readonly cfd: CustomFieldDefinition;
  readonly origin: FieldOrigin;
  /** The item type the definition is attached to; `null` for a lab-wide one. */
  readonly originId: string | null;
  /** The label of that item type, or `null` for a lab-wide definition. */
  readonly originName: string | null;
  /**
   * The less-derived definition of the same key that this one overrides — the
   * one a child "tightens". `null` when the key is defined once.
   */
  readonly tightenedFrom: CustomFieldDefinition | null;
  /**
   * The label of the item type `tightenedFrom` is attached to; `null` when
   * there is nothing to tighten or the shadowed definition is lab-wide.
   */
  readonly tightenedFromName: string | null;
}

/**
 * The definitions that apply to `nodeId`: its own, its ancestors' and the
 * lab-wide ones, one per `key`, the most-derived winning.
 *
 * Same ranking as `storage::resolve_custom_field_defs` — the node itself is the
 * most specific, then its parent, … , then the lab-global definitions at rank 0
 * — and only `SAMPLE`-scoped definitions participate, which is exactly what
 * that function filters on: a box- or freezer-scoped definition attached to an
 * item type is not inherited by a sample, so showing it as an inherited field
 * would be wrong.
 */
export function resolveFields(
  cfds: readonly CustomFieldDefinition[],
  types: readonly ItemType[],
  nodeId: string,
): readonly EffectiveField[] {
  return resolveForLineage(cfds, types, nodeId, lineageOf(types, nodeId));
}

/**
 * What `nodeId` *inherits*: its ancestors' and the lab-wide definitions, one
 * per key, with the node's own definitions left out.
 *
 * This is what the field form checks a new or edited definition against. It
 * cannot be read off `resolveFields`' result on the node itself: once a node
 * defines a key, its own row is the only one left for that key, and the
 * ancestor definition it has to stay compatible with is gone from the list.
 */
export function resolveInheritedFields(
  cfds: readonly CustomFieldDefinition[],
  types: readonly ItemType[],
  nodeId: string,
): readonly EffectiveField[] {
  // Drop the node itself, so its own definitions do not participate in the
  // ranking and lab-global definitions still do.
  const lineage = lineageOf(types, nodeId).slice(1);
  return resolveForLineage(cfds, types, nodeId, lineage);
}

function resolveForLineage(
  cfds: readonly CustomFieldDefinition[],
  types: readonly ItemType[],
  nodeId: string,
  lineage: readonly string[],
): readonly EffectiveField[] {
  const byId = new Map(types.map((type) => [type.id, type]));
  const node = byId.get(nodeId);
  if (node === undefined) {
    return [];
  }
  const rankOf = new Map(lineage.map((id, index) => [id, lineage.length - index]));

  interface Slot {
    rank: number;
    cfd: CustomFieldDefinition;
    shadow: CustomFieldDefinition | null;
  }
  const best = new Map<string, Slot>();

  for (const cfd of cfds) {
    if (cfd.labId !== node.labId || cfd.scopeKind !== ScopeKind.SAMPLE || isArchived(cfd)) {
      continue;
    }
    let rank = 0;
    if (cfd.itemTypeId !== undefined) {
      const found = rankOf.get(cfd.itemTypeId);
      if (found === undefined) {
        continue; // attached to an item type outside this lineage
      }
      rank = found;
    }
    const slot = best.get(cfd.key);
    if (slot === undefined) {
      best.set(cfd.key, { rank, cfd, shadow: null });
    } else if (rank > slot.rank) {
      best.set(cfd.key, { rank, cfd, shadow: slot.cfd });
    } else if (rank === slot.rank) {
      // Two definitions of one key on one node: the resolver's `>=` keeps the
      // last one it sees. Mirroring the tie keeps the screen's answer the same
      // as the form's, instead of picking one at random.
      best.set(cfd.key, { rank, cfd, shadow: slot.shadow });
    }
  }

  return [...best.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, slot]) => ({
      cfd: slot.cfd,
      origin: originOf(slot.cfd, nodeId),
      originId: slot.cfd.itemTypeId ?? null,
      originName:
        slot.cfd.itemTypeId === undefined ? null : (byId.get(slot.cfd.itemTypeId)?.name ?? null),
      tightenedFrom: slot.shadow,
      tightenedFromName:
        slot.shadow?.itemTypeId === undefined
          ? null
          : (byId.get(slot.shadow.itemTypeId)?.name ?? null),
    }));
}

function originOf(cfd: CustomFieldDefinition, nodeId: string): FieldOrigin {
  if (cfd.itemTypeId === undefined) {
    return 'lab';
  }
  return cfd.itemTypeId === nodeId ? 'node' : 'ancestor';
}

export type TightenViolationCode =
  | 'data-type-changed'
  | 'scope-changed'
  | 'required-dropped'
  | 'phi-dropped'
  | 'constraint-dropped'
  | 'constraint-widened';

export interface TightenViolation {
  readonly code: TightenViolationCode;
  /** The field or constraint at fault, e.g. `max_length`; `null` when whole. */
  readonly constraint: string | null;
}

/**
 * What stops `child` from replacing `parent` for the same key.
 *
 * A descendant inherits an ancestor's definition and may *tighten* it — the
 * resolver picks the most-derived definition per key, so "tighten" means the
 * child's definition must not accept anything the parent refuses. An empty list
 * means the replacement is a tightening (or identical).
 *
 * **The server enforces this too, and it is the authoritative half**
 * (`core/custom_field_tightening.h`, called from both custom-field write RPCs,
 * #103): a loosening write from `freezerctl`, the Qt client or anything on
 * REST/gRPC is refused with `INVALID_ARGUMENT`. This copy stays because a form
 * that says *why* before sending beats one that translates a failed request.
 *
 * The rule the issue names explicitly is `required-dropped`: making a required
 * ancestor field optional would drop the requirement for the whole subtree.
 * The permissive direction matters just as much and is the one a careless
 * implementation gets wrong — an optional ancestor field **may** be made
 * required, and a child **may** add a brand-new required field of its own.
 *
 * Indexing is deliberately not part of this: an index is a lookup structure,
 * not a constraint on which values are valid, and L10 forces `indexed` off when
 * a field becomes PHI, so treating "index removed" as a loosening would make
 * the PHI rule unsatisfiable.
 */
export function tightenViolations(
  parent: CustomFieldDefinition,
  child: CustomFieldDefinition,
): readonly TightenViolation[] {
  const violations: TightenViolation[] = [];

  if (parent.dataType !== child.dataType) {
    violations.push({ code: 'data-type-changed', constraint: 'data_type' });
  }
  if (parent.scopeKind !== child.scopeKind) {
    violations.push({ code: 'scope-changed', constraint: 'scope_kind' });
  }
  if (parent.required && !child.required) {
    violations.push({ code: 'required-dropped', constraint: 'required' });
  }
  if (parent.isPhi && !child.isPhi) {
    violations.push({ code: 'phi-dropped', constraint: 'is_phi' });
  }
  violations.push(...validationViolations(parent, child));
  return violations;
}

function validationViolations(
  parent: CustomFieldDefinition,
  child: CustomFieldDefinition,
): readonly TightenViolation[] {
  const before = parseValidation(parent.validationJson);
  const after = parseValidation(child.validationJson);
  const violations: TightenViolation[] = [];

  if (before.maxLength !== undefined) {
    if (after.maxLength === undefined) {
      violations.push({ code: 'constraint-dropped', constraint: 'max_length' });
    } else if (after.maxLength > before.maxLength) {
      violations.push({ code: 'constraint-widened', constraint: 'max_length' });
    }
  }

  if (before.min !== undefined) {
    if (after.min === undefined) {
      violations.push({ code: 'constraint-dropped', constraint: 'min' });
    } else if (compareBound(after.min, before.min) < 0) {
      violations.push({ code: 'constraint-widened', constraint: 'min' });
    }
  }
  if (before.max !== undefined) {
    if (after.max === undefined) {
      violations.push({ code: 'constraint-dropped', constraint: 'max' });
    } else if (compareBound(after.max, before.max) > 0) {
      violations.push({ code: 'constraint-widened', constraint: 'max' });
    }
  }

  if (before.values !== undefined && before.values.length > 0) {
    if (after.values === undefined || after.values.length === 0) {
      violations.push({ code: 'constraint-dropped', constraint: 'values' });
    } else {
      const allowed = new Set(before.values);
      if (after.values.some((value) => !allowed.has(value))) {
        violations.push({ code: 'constraint-widened', constraint: 'values' });
      }
    }
  }

  return violations;
}

/** Numeric when both ends are numbers, lexicographic when either is a string. */
function compareBound(a: number | string, b: number | string): number {
  if (typeof a === 'number' && typeof b === 'number') {
    return a === b ? 0 : a < b ? -1 : 1;
  }
  const left = String(a);
  const right = String(b);
  return left.localeCompare(right);
}

export type ReparentRefusal = 'self' | 'descendant' | 'unchanged';

/**
 * Why `nodeId` may not be re-parented under `newParentId`, or `null` when it
 * may. `null` as the new parent means "move to the root".
 *
 * The guard walks *up* from the drop target carrying a `seen` set: if the walk
 * reaches the dragged node, the target is inside its own subtree and the drop
 * would close a cycle. Walking up also means the guard is exactly as expensive
 * as the chain is deep, and terminates on data that already contains a cycle
 * (which is why the set is there rather than a step counter).
 */
export function canReparent(
  types: readonly ItemType[],
  nodeId: string,
  newParentId: string | null,
): ReparentRefusal | null {
  if (newParentId === null) {
    return null;
  }
  if (newParentId === nodeId) {
    return 'self';
  }
  const byId = new Map(types.map((type) => [type.id, type]));
  if (!byId.has(newParentId)) {
    return null;
  }
  if (byId.get(nodeId)?.parentId === newParentId) {
    return 'unchanged';
  }

  const seen = new Set<string>();
  let cursor: string | undefined = newParentId;
  while (cursor !== undefined && !seen.has(cursor)) {
    if (cursor === nodeId) {
      return 'descendant';
    }
    seen.add(cursor);
    cursor = byId.get(cursor)?.parentId;
  }
  return null;
}

/** The constraints `custom_field_validator.h` implements, as values. */
export interface ValidationConstraints {
  readonly maxLength?: number;
  readonly min?: number | string;
  readonly max?: number | string;
  readonly values?: readonly string[];
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** Known constraints from a `validation_json` string. Anything else is ignored. */
export function parseValidation(json: string): ValidationConstraints {
  if (json.trim() === '') {
    return {};
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    return {};
  }
  if (!isRecord(parsed)) {
    return {};
  }

  const constraints: {
    maxLength?: number;
    min?: number | string;
    max?: number | string;
    values?: readonly string[];
  } = {};
  const maxLength = parsed.max_length;
  if (typeof maxLength === 'number' && Number.isFinite(maxLength)) {
    constraints.maxLength = maxLength;
  }
  const min = parsed.min;
  if (typeof min === 'number' || typeof min === 'string') {
    constraints.min = min;
  }
  const max = parsed.max;
  if (typeof max === 'number' || typeof max === 'string') {
    constraints.max = max;
  }
  const values = parsed.values;
  if (
    Array.isArray(values) &&
    values.every((value): value is string => typeof value === 'string')
  ) {
    constraints.values = values;
  }
  return constraints;
}

const KNOWN_KEYS = ['max_length', 'min', 'max', 'values'] as const;

/**
 * The `validation_json` string for a definition of `dataType`.
 *
 * Only the constraints that apply to the data type are written: the validator
 * ignores the rest, and a `max_length` on an integer reads as a rule that is
 * enforced when it is not. Constraints written by a newer server that this
 * bundle does not know are carried through from `previousJson` untouched —
 * renaming a field must not silently delete a rule the editor cannot show.
 */
export function serializeValidation(
  constraints: ValidationConstraints,
  dataType: FieldDataType,
  previousJson = '{}',
): string {
  const previous: Record<string, unknown> = (() => {
    try {
      const parsed: unknown = JSON.parse(previousJson);
      return isRecord(parsed) ? parsed : {};
    } catch {
      return {};
    }
  })();
  // Drop the constraints this writer owns and carry everything else through:
  // a rule a newer server wrote must survive a rename in this bundle.
  const base: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(previous)) {
    if (!KNOWN_KEYS.includes(key as (typeof KNOWN_KEYS)[number])) {
      base[key] = value;
    }
  }

  switch (dataType) {
    case FieldDataType.TEXT:
      if (constraints.maxLength !== undefined) {
        base.max_length = constraints.maxLength;
      }
      break;
    case FieldDataType.INT:
    case FieldDataType.FLOAT:
      if (typeof constraints.min === 'number') {
        base.min = constraints.min;
      }
      if (typeof constraints.max === 'number') {
        base.max = constraints.max;
      }
      break;
    case FieldDataType.DATE:
    case FieldDataType.DATETIME:
      if (typeof constraints.min === 'string' && constraints.min !== '') {
        base.min = constraints.min;
      }
      if (typeof constraints.max === 'string' && constraints.max !== '') {
        base.max = constraints.max;
      }
      break;
    case FieldDataType.ENUM:
      if (constraints.values !== undefined && constraints.values.length > 0) {
        base.values = [...constraints.values];
      }
      break;
    case FieldDataType.BOOL:
    case FieldDataType.REFERENCE:
    case FieldDataType.UNSPECIFIED:
      break;
  }

  return JSON.stringify(base);
}

/** The definition form's fields, as the editor holds them. */
export interface DefinitionDraft {
  readonly key: string;
  readonly label: string;
  readonly dataType: FieldDataType;
  readonly required?: boolean;
  readonly indexed?: boolean;
  readonly isPhi?: boolean;
  readonly maxLength?: number;
  readonly min?: number | string;
  readonly max?: number | string;
  readonly values?: readonly string[];
}

export type DefinitionProblemCode =
  | 'key-required'
  | 'label-required'
  | 'phi-and-indexed'
  | 'phi-not-enabled'
  | 'enum-values-required'
  | 'enum-values-duplicated'
  | 'range-inverted';

export interface DefinitionProblem {
  readonly code: DefinitionProblemCode;
}

export interface DefinitionContext {
  /** The lab's PHI mode (`Lab.is_phi_enabled`, G0.2's `WhoAmI`). */
  readonly phiEnabled: boolean;
}

/**
 * What makes this definition unsaveable on its own.
 *
 * `phi-and-indexed` is the L10 rule: a JSON-path index stores the *plaintext*
 * value outside the encryption layer, so indexing a PHI field would defeat the
 * encryption it is supposed to have. The server refuses it too
 * (`ItemTypeServiceImpl::reject_indexed_phi` and
 * `detail::validate_cfd_shape`); this is the same refusal early enough to say
 * why in the form instead of in a failed request.
 *
 * `phi-not-enabled` is the UI's half of "`is_phi` is offered only when the lab
 * has PHI mode on". The server does not check the lab flag when a definition is
 * created — it checks it when PHI *values* are written
 * (`SampleServiceImpl::prepare_custom_fields`) — so a definition that arrives
 * with `is_phi` while the lab is in normal mode is one whose values will be
 * refused later. Refusing it at the definition is the honest place.
 */
export function definitionProblems(
  draft: DefinitionDraft,
  context: DefinitionContext,
): readonly DefinitionProblem[] {
  const problems: DefinitionProblem[] = [];
  if (draft.key.trim() === '') {
    problems.push({ code: 'key-required' });
  }
  if (draft.label.trim() === '') {
    problems.push({ code: 'label-required' });
  }
  if (draft.isPhi === true && draft.indexed === true) {
    problems.push({ code: 'phi-and-indexed' });
  }
  if (draft.isPhi === true && !context.phiEnabled) {
    problems.push({ code: 'phi-not-enabled' });
  }
  if (draft.dataType === FieldDataType.ENUM) {
    const values = draft.values ?? [];
    if (values.length === 0) {
      problems.push({ code: 'enum-values-required' });
    } else if (new Set(values).size !== values.length) {
      problems.push({ code: 'enum-values-duplicated' });
    }
  }
  if (
    draft.min !== undefined &&
    draft.max !== undefined &&
    compareBound(draft.min, draft.max) > 0
  ) {
    problems.push({ code: 'range-inverted' });
  }
  return problems;
}

/** The constraints a draft carries, in the shape `serializeValidation` wants. */
export function draftConstraints(draft: DefinitionDraft): ValidationConstraints {
  return {
    maxLength: draft.maxLength,
    min: draft.min,
    max: draft.max,
    values: draft.values,
  };
}
