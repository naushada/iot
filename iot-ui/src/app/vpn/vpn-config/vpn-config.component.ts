import { Component, OnInit } from '@angular/core';
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
export class VpnConfigComponent implements OnInit {
  form: FormGroup;
  loading = true;
  get isAdmin(): boolean { return this.session.isAdmin; }
  saving = false;
  msg = '';

  constructor(private http: HttpsvcService, fb: FormBuilder,
    private session: SessionService, private toast: ToastService,
    private ds: DataStoreService) {
    this.form = fb.group({
      remote_host:  [''],
      remote_port:  [1194],
      remote_proto: ['udp'],
      cert_path:    [''],
      key_path:     [''],
      ca_path:      [''],
      cipher:       ['AES-256-GCM'],
      dev:          ['tun'],
      mgmt_port:    [7505],
    });
  }

  ngOnInit(): void {
    // Instant paint from the prefetched cache, then refresh from the wire.
    if (this.ds.has('vpn.remote.host')) { this.applyData(this.ds.snapshot()); this.loading = false; }
    this.http.dbGet([
      'vpn.remote.host', 'vpn.remote.port', 'vpn.remote.proto',
      'vpn.cert.path', 'vpn.key.path', 'vpn.ca.path',
      'vpn.cipher', 'vpn.dev', 'vpn.mgmt.port'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) this.applyData(r.data as Record<string, unknown>);
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  private applyData(d: Record<string, unknown>): void {
    this.form.patchValue({
      remote_host:  d['vpn.remote.host']  ?? this.form.value.remote_host,
      remote_port:  d['vpn.remote.port']  ?? this.form.value.remote_port,
      remote_proto: d['vpn.remote.proto'] ?? this.form.value.remote_proto,
      cert_path:    d['vpn.cert.path']    ?? this.form.value.cert_path,
      key_path:     d['vpn.key.path']     ?? this.form.value.key_path,
      ca_path:      d['vpn.ca.path']      ?? this.form.value.ca_path,
      cipher:       d['vpn.cipher']       ?? this.form.value.cipher,
      dev:          d['vpn.dev']          ?? this.form.value.dev,
      mgmt_port:    d['vpn.mgmt.port']    ?? this.form.value.mgmt_port,
    });
  }

  save(): void {
    this.saving = true; this.msg = '';
    const v = this.form.value;
    this.http.dbSet([
      { key: 'vpn.remote.host',  value: v.remote_host },
      { key: 'vpn.remote.port',  value: v.remote_port },
      { key: 'vpn.remote.proto', value: v.remote_proto },
      { key: 'vpn.cert.path',    value: v.cert_path },
      { key: 'vpn.key.path',     value: v.key_path },
      { key: 'vpn.ca.path',      value: v.ca_path },
      { key: 'vpn.cipher',       value: v.cipher },
      { key: 'vpn.dev',          value: v.dev },
      { key: 'vpn.mgmt.port',    value: v.mgmt_port },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if(r.ok) this.toast.success('VPN config saved'); else this.toast.error(r.err||'Save failed');
      },
      error: (e) => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
