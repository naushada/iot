import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

@Component({
  selector: 'app-vpn-config',
  templateUrl: './vpn-config.component.html',
  styleUrls: ['./vpn-config.component.scss']
})
export class VpnConfigComponent implements OnInit, OnDestroy {
  form: FormGroup;
  loading = true;
  get isAdmin(): boolean { return this.session.isAdmin; }
  saving = false;
  private sub = new Subscription();
  // Cached (prefetched) keys only. cloud.vpn.ca.key / server.key are kept OUT
  // of the shared cache (secret-adjacent paths) and fetched on demand below.
  private readonly KEYS = [
    'cloud.vpn.subnet', 'cloud.vpn.port.next', 'cloud.vpn.listen.port',
    'cloud.vpn.proto', 'cloud.vpn.cipher', 'cloud.vpn.dev', 'cloud.vpn.mgmt.port',
    'cloud.vpn.verb', 'cloud.vpn.ca.crt', 'cloud.vpn.server.crt',
  ];

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService,
    private ds: DataStoreService
  ) {
    this.form = fb.group({
      subnet:      ['10.9.0.0/24'],
      port_next:   [5001],
      listen_port: [1194],
      proto:       ['udp'],
      cipher:      ['AES-256-GCM'],
      dev:         ['tun'],
      mgmt_port:   [7506],
      verb:        [3],
      ca_crt:     ['/etc/iot/vpn/ca/ca.crt'],
      ca_key:     ['/run/secrets/iot-ca-key/ca.key'],
      server_crt: ['/etc/iot/vpn/server.crt'],
      server_key: ['/etc/iot/vpn/server.key'],
    });
  }

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache (no per-page round-trip),
    // then stay live off the appglobal store; re-apply only while the form is
    // pristine so a late prefetch fills the fields without clobbering edits.
    this.applyData(this.ds.snapshot());
    this.loading = false;
    for (const k of this.KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.form.dirty) this.applyData(this.ds.snapshot());
      }));
    // ca.key / server.key are deliberately excluded from the prefetch cache
    // (secret-adjacent key paths), so fetch just those two on demand.
    this.http.dbGet(['cloud.vpn.ca.key', 'cloud.vpn.server.key']).subscribe({
      next: (r) => { if (r.ok && r.data && !this.form.dirty) this.applyData(r.data as Record<string, unknown>); }
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    this.form.patchValue({
      subnet:      d['cloud.vpn.subnet']      ?? this.form.value.subnet,
      port_next:   d['cloud.vpn.port.next']   ?? this.form.value.port_next,
      listen_port: d['cloud.vpn.listen.port'] ?? this.form.value.listen_port,
      proto:       d['cloud.vpn.proto']       ?? this.form.value.proto,
      cipher:      d['cloud.vpn.cipher']      ?? this.form.value.cipher,
      dev:         d['cloud.vpn.dev']         ?? this.form.value.dev,
      mgmt_port:   d['cloud.vpn.mgmt.port']   ?? this.form.value.mgmt_port,
      verb:        d['cloud.vpn.verb']        ?? this.form.value.verb,
      ca_crt:      d['cloud.vpn.ca.crt']      ?? this.form.value.ca_crt,
      ca_key:      d['cloud.vpn.ca.key']      ?? this.form.value.ca_key,
      server_crt:  d['cloud.vpn.server.crt']  ?? this.form.value.server_crt,
      server_key:  d['cloud.vpn.server.key']  ?? this.form.value.server_key,
    });
  }

  save(): void {
    this.saving = true;
    const v = this.form.value;
    // Secret-adjacent key paths are kept out of the shared cache → plain dbSet.
    this.http.dbSet([
      { key: 'cloud.vpn.ca.key',      value: v.ca_key },
      { key: 'cloud.vpn.server.key',  value: v.server_key },
    ]).subscribe({ error: () => this.toast.error('Failed to save key paths') });
    // Cached config keys go through the shared store so the cache stays fresh.
    this.ds.write([
      { key: 'cloud.vpn.subnet',      value: v.subnet },
      { key: 'cloud.vpn.port.next',   value: v.port_next },
      { key: 'cloud.vpn.listen.port', value: v.listen_port },
      { key: 'cloud.vpn.proto',       value: v.proto },
      { key: 'cloud.vpn.cipher',      value: v.cipher },
      { key: 'cloud.vpn.dev',         value: v.dev },
      { key: 'cloud.vpn.mgmt.port',   value: v.mgmt_port },
      { key: 'cloud.vpn.verb',        value: v.verb },
      { key: 'cloud.vpn.ca.crt',      value: v.ca_crt },
      { key: 'cloud.vpn.server.crt',  value: v.server_crt },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('VPN config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
