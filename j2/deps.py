"""Liste les dépôts git d'un fichier DEPS de Chromium actifs pour une configuration.

Le DEPS est lu comme une donnée : arbre syntaxique Python évalué sur un sous-ensemble
restreint (chaînes, nombres, booléens, listes, dictionnaires, concaténation, Var(), Str()),
jamais exécuté. Les conditions (« checkout_win and not checkout_nacl ») sont évaluées de
la même façon.

Sortie, une ligne par dépôt : chemin<TAB>url<TAB>révision. Les recursedeps sont listés
en commentaires (« # recursedeps chemin »), pour que l'appelant récupère leur DEPS.

  python -I deps.py DEPS [--prefix third_party/angle] [--var checkout_nacl=False ...]
"""

import ast
import sys

# Variables intégrées de gclient pour notre cas : hôte Windows x64, cible Windows.
BUILTINS = {
    'checkout_android': False, 'checkout_chromeos': False, 'checkout_fuchsia': False,
    'checkout_ios': False, 'checkout_linux': False, 'checkout_mac': False,
    'checkout_win': True, 'host_os': 'win', 'host_cpu': 'x64',
    'checkout_arm': False, 'checkout_arm64': False, 'checkout_x86': False,
    'checkout_x64': True, 'checkout_mips': False, 'checkout_mips64': False,
    'checkout_ppc': False, 'checkout_s390': False, 'checkout_loong64': False,
    'checkout_riscv64': False, 'non_git_source': True, 'build_with_chromium': True,
}


def evaluate(node, variables):
  if isinstance(node, ast.Constant):
    return node.value
  if isinstance(node, ast.Dict):
    return {evaluate(k, variables): evaluate(v, variables)
            for k, v in zip(node.keys, node.values)}
  if isinstance(node, (ast.List, ast.Tuple)):
    return [evaluate(e, variables) for e in node.elts]
  if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
    return evaluate(node.left, variables) + evaluate(node.right, variables)
  if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
      and len(node.args) == 1 and not node.keywords):
    arg = evaluate(node.args[0], variables)
    if node.func.id == 'Var':
      value = variables.get(arg, '{%s}' % arg)
      return str(value) if not isinstance(value, bool) else value
    if node.func.id == 'Str':
      return arg
  raise ValueError('construction non prise en charge : %s' % ast.dump(node)[:80])


def condition_true(text, variables):
  if not text:
    return True

  def walk(node):
    if isinstance(node, ast.Expression):
      return walk(node.body)
    if isinstance(node, ast.BoolOp):
      values = [walk(v) for v in node.values]
      return all(values) if isinstance(node.op, ast.And) else any(values)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
      return not walk(node.operand)
    if isinstance(node, ast.Compare) and len(node.ops) == 1:
      left, right = walk(node.left), walk(node.comparators[0])
      if isinstance(node.ops[0], ast.Eq):
        return left == right
      if isinstance(node.ops[0], ast.NotEq):
        return left != right
    if isinstance(node, ast.Name):
      return variables.get(node.id, False)
    if isinstance(node, ast.Constant):
      return node.value
    raise ValueError('condition non prise en charge : %s' % text)

  return walk(ast.parse(text, mode='eval'))


def main(argv):
  path = argv[1]
  prefix = ''
  overrides = {}
  i = 2
  while i < len(argv):
    if argv[i] == '--prefix':
      prefix = argv[i + 1].strip('/') + '/'
      i += 2
    elif argv[i] == '--var':
      name, value = argv[i + 1].split('=', 1)
      overrides[name] = {'True': True, 'False': False}.get(value, value)
      i += 2
    else:
      raise SystemExit('argument inconnu : %s' % argv[i])

  with open(path, encoding='utf-8') as f:
    tree = ast.parse(f.read(), filename=path)

  top = {}
  for statement in tree.body:
    if (isinstance(statement, ast.Assign) and len(statement.targets) == 1
        and isinstance(statement.targets[0], ast.Name)
        and statement.targets[0].id in ('vars', 'deps', 'recursedeps',
                                        'use_relative_paths')):
      top[statement.targets[0].id] = statement.value

  variables = dict(BUILTINS)
  if 'vars' in top:
    raw = evaluate(top['vars'], {})
    variables.update(raw)
  variables.update(overrides)
  # Les variables chaînes peuvent en contenir d'autres ({chromium_git}…).
  for _ in range(3):
    for name, value in list(variables.items()):
      if isinstance(value, str) and '{' in value:
        try:
          variables[name] = value.format(**variables)
        except (KeyError, IndexError, ValueError):
          pass

  relative = bool(top.get('use_relative_paths')
                  and evaluate(top['use_relative_paths'], variables))
  deps = evaluate(top.get('deps', ast.Dict(keys=[], values=[])), variables)
  for name, spec in sorted(deps.items()):
    condition = ''
    if isinstance(spec, dict):
      if spec.get('dep_type', 'git') != 'git':
        continue
      condition = spec.get('condition', '')
      url = spec.get('url')
    else:
      url = spec
    if not url or not condition_true(condition, variables):
      continue
    url = url.format(**variables) if '{' in url else url
    repo, _, revision = url.partition('@')
    local = name if relative else name[len('src/'):] if name.startswith('src/') else name
    print('%s%s\t%s\t%s' % (prefix, local, repo, revision))

  for entry in evaluate(top['recursedeps'], variables) if 'recursedeps' in top else []:
    name = entry[0] if isinstance(entry, list) else entry
    local = name if relative else name[len('src/'):] if name.startswith('src/') else name
    print('# recursedeps %s%s' % (prefix, local))


if __name__ == '__main__':
  main(sys.argv)
