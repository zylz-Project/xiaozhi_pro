const PRODUCTS = {
  crawlPanda: {
    id: 'crawlPanda',
    name: '爬爬熊猫',
    short: '熊猫',
    english: 'CRAWL PANDA',
    code: 'PANDA',
    ap: 'CrawlPanda',
    hot: '1A2B',
    theme: 'milk',
    model: 'WZH-CP01',
    role: '小华',
    wake: '小华小华',
    onboardingProfile: 'an',
    icon: '/assets/crawlPanda_icon.png',
    voice: true
  },
  dinosaur: {
    id: 'dinosaur',
    name: '小恐龙',
    short: '恐龙',
    english: 'LITTLE DINO',
    code: 'DINO',
    ap: 'Dino',
    hot: '6C3F',
    theme: 'bamboo',
    model: 'WZH-DN01',
    role: '小威',
    wake: '小威小威',
    onboardingProfile: 'long',
    icon: '/assets/dinosaur_icon.png',
    voice: true
  },
  redpanda: {
    id: 'redpanda',
    name: '摇尾小熊猫',
    short: '小熊猫',
    english: 'TAIL RED PANDA',
    code: 'RED PANDA',
    ap: 'RedPanda',
    hot: '9D2E',
    theme: 'berry',
    model: 'WZH-RP01',
    role: '小武',
    wake: '小武小武',
    onboardingProfile: 'xing',
    icon: '/assets/tailRedPanda_icon.png',
    voice: true
  }
}

const ALIASES = {
  panda: 'crawlPanda',
  crawlPanda: 'crawlPanda',
  dinosaur: 'dinosaur',
  redpanda: 'redpanda',
  tailRedPanda: 'redpanda',
  wzh_tailRedPanda: 'redpanda',
  'crawlpanda': 'crawlPanda'
}

function normalizeId(raw) {
  return ALIASES[raw] || (PRODUCTS[raw] ? raw : 'crawlPanda')
}

function get(id) {
  return PRODUCTS[normalizeId(id)]
}

function matchName(name) {
  const n = String(name || '').toLowerCase()
  if (n.indexOf('dino') >= 0 || n.indexOf('恐龙') >= 0) return PRODUCTS.dinosaur
  if (n.indexOf('redpanda') >= 0 || n.indexOf('tailred') >= 0 || n.indexOf('小熊猫') >= 0) return PRODUCTS.redpanda
  if (n.indexOf('crawlpanda') >= 0 || n.indexOf('爬爬熊猫') >= 0) return PRODUCTS.crawlPanda
  return fromModel(n)
}

function fromName(name) {
  return matchName(name) || PRODUCTS.crawlPanda
}

const MODEL_TO_ID = {
  'WZH-CP01': 'crawlPanda',
  'WZH-DN01': 'dinosaur',
  'WZH-RP01': 'redpanda'
}

function fromModel(model) {
  const id = MODEL_TO_ID[String(model || '').toUpperCase()]
  return id ? PRODUCTS[id] : null
}

function detectProduct(device) {
  if (!device) return PRODUCTS.crawlPanda
  if (device.productId) {
    const byModel = fromModel(device.productId)
    if (byModel) return byModel
  }
  if (device.model) {
    const byModel = fromModel(device.model)
    if (byModel) return byModel
  }
  if (device.product && PRODUCTS[device.product]) return PRODUCTS[device.product]
  return fromName(device.product || device.name || device.localName || '')
}

module.exports = { PRODUCTS, ALIASES, normalizeId, get, matchName, fromName, fromModel, detectProduct }
