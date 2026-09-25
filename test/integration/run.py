#!/usr/bin/env python3
"""Real HTTP exchanges using the firmware's native protocol/policy probe."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import unittest
import uuid

from seed.kosync import BASE_URL, AdminClient, KosyncDevice, request_json, partial_md5, kodatetime

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((Path(__file__).parent / 'seed-manifest.json').read_text())
PROBE = Path(os.environ.get('BOOKORBIT_PROTOCOL_PROBE', ROOT / 'build/test/bookorbit_data/BookOrbitProtocolProbe'))

class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = subprocess.Popen([str(PROBE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        cls.run_id = uuid.uuid4().hex[:12]
        cls.a = KosyncDevice(BASE_URL, 'crossink-sync', 'CrossinkSync1', 'crosspoint-a-' + cls.run_id)
        cls.b = KosyncDevice(BASE_URL, 'crossink-sync', 'CrossinkSync1', 'crosspoint-b-' + cls.run_id)
        cls.a.auth(); cls.b.auth()
        cls.hash = partial_md5(Path(__file__).parent / 'library/integration-book-090.epub')

    @classmethod
    def tearDownClass(cls):
        cls.probe.stdin.close(); cls.probe.wait(timeout=5); cls.probe.stdout.close()

    def production(self, **payload):
        self.probe.stdin.write(json.dumps(payload) + '\n'); self.probe.stdin.flush()
        return json.loads(self.probe.stdout.readline())

    def post(self, device, route, payload):
        return request_json('POST', device.api('/plugin/' + route), device._headers(), payload)

    def exchange(self, device, bookmark=False, keys=(), changes=(), complete=False):
        payload = self.production(op='exchange', deviceId=device.device_id, hash=self.hash,
                                  bookmark=bookmark, keys=list(keys), changes=list(changes), complete=complete)
        route = 'bookmarks' if bookmark else 'annotations'
        response = self.post(device, route + '/exchange', payload)
        parsed = self.production(op='parse', body=json.dumps(response))
        self.assertTrue(parsed['complete']); self.assertFalse(parsed['unmatched'])
        return parsed, response

    def test_01_two_device_conflict_and_backward_reading(self):
        before = self.a.put_progress(self.hash, '/body/DocFragment[1]/body/p[1]/text().10', .1)
        remote = self.b.put_progress(self.hash, '/body/DocFragment[1]/body/p[1]/text().80', .8)
        fetched = self.a.get_progress(self.hash)
        self.assertEqual(fetched['device_id'], self.b.device_id)
        base = dict(op='decision', ackLocal=10, ackRemote=10, ackTimestamp=int(before['timestamp']),
                    remoteTimestamp=int(remote['timestamp']), remote=80)
        self.assertEqual(self.production(**base, local=40)['decision'], 3)
        self.assertEqual(self.production(**base, local=5)['decision'], 3)
        base.update(remote=10, remoteTimestamp=int(before['timestamp']))
        self.assertEqual(self.production(**base, local=5)['decision'], 1)

    def test_02_reading_stats_lost_response_retry(self):
        payload = self.production(op='stats', deviceId=self.a.device_id, hash=self.hash,
                                  events=[dict(page=1234, startTime=int(time.time()) - 600,
                                               durationSeconds=17, totalPages=10000)])
        first = self.post(self.a, 'page-stats', payload)
        retry = self.post(self.a, 'page-stats', payload)
        for response in [first, retry]:
            self.assertTrue(self.production(op='stats-accepted', body=json.dumps(response), hash=self.hash, count=1)['accepted'])
        self.assertEqual(retry['results'][0]['duplicates'], 1)
        self.assertFalse(self.production(op='stats-accepted', body=json.dumps(retry), hash=self.hash, count=2)['accepted'])

    def test_03_annotations_retry_recovery_guard_and_delete_last(self):
        dt = kodatetime(int(time.time()) - 3600)
        pos = '/body/DocFragment[1]/body/p[1]/text().5'
        key = hashlib.md5((dt + '|' + pos).encode()).hexdigest()
        change = dict(datetime=dt, pos=pos, end='/body/DocFragment[1]/body/p[1]/text().18', text='test ' + self.run_id)
        keys = [dict(k=key, dt=dt)]
        self.exchange(self.a, keys=keys, changes=[change])
        self.exchange(self.a, keys=keys, changes=[change])
        pulled, _ = self.exchange(self.b)
        matches = [x for x in pulled['entries'] if x['text'] == change['text']]
        self.assertEqual(len(matches), 1)
        ack = self.production(op='ack', deviceId=self.b.device_id, hash=self.hash, entries=matches)
        self.post(self.b, 'annotations/exchange-ack', ack)
        self.post(self.b, 'annotations/exchange-ack', ack)  # response lost after apply
        change['text'] += ' edited'
        change['updated'] = kodatetime(int(time.time()))
        self.exchange(self.a, keys=keys, changes=[change])
        edited, _ = self.exchange(self.b)
        self.assertTrue(any(x['serverId'] == matches[0]['serverId'] and x['text'] == change['text'] for x in edited['entries']))
        # Losing local state must not be interpreted as deletion.
        self.exchange(self.a, complete=False)
        again, _ = self.exchange(self.b)
        self.assertTrue(any(x['text'] == change['text'] and not x['deleted'] for x in again['entries']))
        # A device with a known, complete empty inventory intentionally deletes its last item.
        _, deleted = self.exchange(self.a, complete=True)
        self.assertGreaterEqual(deleted['results'][0]['applied']['deviceDeleted'], 1)
        gone, _ = self.exchange(self.b)
        tombstones = [x for x in gone['entries'] if x['serverId'] == matches[0]['serverId'] and x['deleted']]
        self.assertEqual(len(tombstones), 1)
        ack = self.production(op='ack', deviceId=self.b.device_id, hash=self.hash, entries=tombstones)
        self.post(self.b, 'annotations/exchange-ack', ack)

    def test_04_bookmark_identity_and_ack_retry(self):
        dt = kodatetime(int(time.time()) - 7200)
        pos = '/body/DocFragment[1]/body/p[1]/text().21'
        key = hashlib.md5((dt + '|' + pos).encode()).hexdigest()
        self.exchange(self.a, bookmark=True, keys=[dict(k=key, dt=dt)], changes=[dict(datetime=dt, pos=pos)])
        pulled, _ = self.exchange(self.b, bookmark=True)
        entry = next(x for x in pulled['entries'] if x['pos'] == pos and not x['deleted'])
        payload = dict(deviceId=self.b.device_id, deviceModel='Host integration', pluginVersion='crosspoint-bo-2',
                       books=[dict(hash=self.hash, applied=[dict(serverId=entry['serverId'], status='applied',
                                  key=key, datetime=dt, pos=pos)], deleted=[])])
        self.post(self.b, 'bookmarks/exchange-ack', payload)
        self.post(self.b, 'bookmarks/exchange-ack', payload)
        again, _ = self.exchange(self.b, bookmark=True, keys=[dict(k=key, dt=dt)])
        self.assertFalse(any(x['serverId'] == entry['serverId'] for x in again['entries']))

    def test_05_sideload_exact_hash_matching(self):
        body = dict(deviceId=self.a.device_id, deviceModel='Host integration', pluginVersion='crosspoint-bo-2',
                    hashes=[self.hash, '0' * 32])
        response = self.post(self.a, 'match-check', body)
        self.assertTrue(any(x['hash'] == self.hash and x['bookId'] > 0 and x['bookFileId'] > 0 for x in response['matches']))
        self.assertFalse(any(x['hash'] == '0' * 32 for x in response['matches']))

    def test_06_catalog_pages_search_and_collections(self):
        def get(route):
            return request_json('GET', self.a.api('/plugin/catalog/' + route), self.a._headers())
        first = get('books?page=1&size=20&format=epub&sort=title')
        last = get('books?page=5&size=20&format=epub&sort=title')
        self.assertGreaterEqual(first['total'], 100)
        self.assertFalse({x['id'] for x in first['items']} & {x['id'] for x in last['items']})
        # Dock imports can exist before metadata extraction. Seed a searchable
        # metadata title explicitly so the query assertion does not race it.
        admin = AdminClient(BASE_URL)
        admin.login('crossink', 'CrossinkTest1')
        match = self.post(self.a, 'match-check', dict(deviceId=self.a.device_id, deviceModel='Host integration', pluginVersion='crosspoint-bo-2', hashes=[self.hash]))
        protected_id = match['matches'][0]['bookId']
        fixture_id = next(x['id'] for x in first['items'] if x['id'] != protected_id)
        request_json('PATCH', admin.api('/books/' + str(fixture_id) + '/metadata'),
                     admin._headers(), {'title': 'CrossPoint search fixture ' + self.run_id})
        found = get('books?page=1&size=20&format=epub&q=' + self.run_id)
        self.assertEqual(len(found['items']), 1)
        collections = get('sections/collections?page=1')
        shelf = next(x for x in collections['items'] if x['title'] == 'Integration Shelf')
        books = get('books?page=1&size=20&format=epub&collectionId=' + str(shelf['id']))
        self.assertGreater(books['total'], 0)
        empty = next(x for x in collections['items'] if x['title'] == 'Zero Shelf')
        self.assertEqual(get('books?page=1&size=20&format=epub&collectionId=' + str(empty['id']))['total'], 0)

if __name__ == '__main__':
    unittest.main(verbosity=2)
