"""Lock the /api/network/transport dispatch layout in the portal handler.

The transport endpoint used to sit inside the `if (req->method == HTTP_GET)`
block, so a POST fell through to the catch-all 404 and the Wi-Fi/4G switch
never ran on the device. Browser tests could not catch it because their mock
server answers the POST itself. These checks read the real Handle() and assert
where each half of the endpoint is reachable from.
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'main/boards/common/wzh/wzh_user_web.cc'


def find_handle(source):
    start = source.index('esp_err_t UserWebPortal::Handle(')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def depth_at(source, position, handle_start=0):
    """Brace depth of `position` relative to the opening brace of Handle()."""
    opening = source.index('{', source.index('esp_err_t UserWebPortal::Handle('))
    body = source[opening + 1:position]
    # Ignore preprocessor lines: their braces are not C++ scope.
    body = '\n'.join(line for line in body.splitlines() if not line.lstrip().startswith('#'))
    return body.count('{') - body.count('}')


class TransportEndpointTest(unittest.TestCase):
    def setUp(self):
        self.source = SOURCE.read_text()
        self.handle = find_handle(self.source)
        self.get_block = self.source.index('if (req->method == HTTP_GET) {',
                                           self.source.index('esp_err_t UserWebPortal::Handle('))
        self.method_guard = self.source.index('if (req->method != HTTP_POST)')
        self.handle_end = self.source.index(self.handle) + len(self.handle)

    def transport_blocks(self):
        marker = 'if (path == "/api/network/transport") {'
        blocks, start = [], self.handle_start()
        while True:
            try:
                start = self.source.index(marker, start)
            except ValueError:
                return blocks
            end = start + len(marker)
            depth = 1
            while depth:
                depth += (self.source[end] == '{') - (self.source[end] == '}')
                end += 1
            blocks.append((start, self.source[start:end]))
            start = end

    def handle_start(self):
        return self.source.index('esp_err_t UserWebPortal::Handle(')

    def test_both_halves_are_reachable(self):
        blocks = self.transport_blocks()
        self.assertEqual(len(blocks), 2,
                         'the endpoint must have one GET half and one POST half')
        gets = [b for b in blocks if 'SelectNetworkTransport' not in b[1]]
        posts = [b for b in blocks if 'SelectNetworkTransport' in b[1]]
        self.assertEqual(len(gets), 1, 'one read-only status half expected')
        self.assertEqual(len(posts), 1, 'one mutating half expected')

        get_pos, get_body = gets[0]
        post_pos, post_body = posts[0]
        # The status half belongs to the GET branch. depth_at counts the scope
        # the statement itself lives in, so the GET block body is depth 1 and
        # Handle() top level is depth 0.
        self.assertGreater(get_pos, self.get_block)
        self.assertEqual(depth_at(self.source, get_pos), 1,
                         'GET half must sit directly inside the HTTP_GET block')
        # The mutating half must be a top-level POST branch.
        self.assertGreater(post_pos, self.method_guard)
        self.assertLess(post_pos, self.handle_end)
        self.assertEqual(depth_at(self.source, post_pos), 0,
                         'POST half must sit at Handle() top level, not inside the GET block')
        self.assertIn('std::strcmp(transport, "cellular")', post_body)
        self.assertIn('Ok(req', post_body)

    def test_switch_does_not_require_an_online_transport(self):
        # The NET_OFFLINE gate exists for voice management; a transport switch is
        # how the user recovers from an offline selection, so it must precede it.
        post_pos = next(p for p, b in self.transport_blocks()
                        if 'SelectNetworkTransport' in b)
        offline_gate = self.source.index('"设备暂未联网"', post_pos)
        self.assertLess(post_pos, offline_gate,
                        'the switch must be handled before the NET_OFFLINE gate')

    def test_status_poll_is_never_session_gated(self):
        # Switching transport drops the audio channel and may drop the AP
        # session. The detail page keeps polling /api/status through the
        # switch, so those polls must answer with the transport fields (not
        # 401), or the page bounces to /login instead of staying on the
        # device-detail view.
        status_start = self.source.index('if (path == "/api/status") {',
                                         self.handle_start())
        status_end = self.source.index('\n        }\n', status_start)
        block = self.source[status_start:status_end]
        self.assertNotIn('if (!session)', block,
                         '/api/status must answer transport polls without a session')
        self.assertIn('"transport"', block)
        self.assertIn('"transport_switchable"', block)

    def test_offline_dinosaur_routes_through_login(self):
        # A 4G to Wi-Fi switch can change the AP channel and invalidate the
        # phone's old address. Offline access without a session must go to the
        # login page, not back to the Wi-Fi setup flow, or the detail page is
        # unreachable until the user manually guesses /login.
        self.assertIn('AllowOfflineLogin', self.handle)
        self.assertIn('const bool offline_login', self.handle)
        route_start = self.source.index('UserWebPolicy::RouteFor(')
        route_end = self.source.index('offline_login);', route_start) + len('offline_login);')
        self.assertIn('offline_login', self.source[route_start:route_end])


if __name__ == '__main__':
    unittest.main()
