import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';

@Component({
  selector: 'app-lwm2m-config',
  templateUrl: './lwm2m-config.component.html',
  styleUrls: ['./lwm2m-config.component.scss']
})
export class Lwm2mConfigComponent implements OnInit {
  serverForm: FormGroup;
  loading = true; saving = false; msg = '';

    get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, fb: FormBuilder, private session: SessionService) {
    this.serverForm = fb.group({
      server_uri: ['coaps://'],
      endpoint:   ['urn:dev:client-1'],
      binding:    ['U'],
      lifetime:   [86400],
      observable: [true],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'iot.server.uri', 'iot.endpoint', 'iot.binding',
      'iot.lifetime', 'iot.observable'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.serverForm.patchValue({
            server_uri: d['iot.server.uri'] || 'coaps://',
            endpoint:   d['iot.endpoint']   || 'urn:dev:client-1',
            binding:    d['iot.binding']    || 'U',
            lifetime:   d['iot.lifetime']   || 86400,
            observable: d['iot.observable'] ?? true,
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  save(): void {
    this.saving = true; this.msg = '';
    const v = this.serverForm.value;
    this.http.dbSet([
      { key: 'iot.server.uri', value: v.server_uri },
      { key: 'iot.endpoint',   value: v.endpoint },
      { key: 'iot.binding',    value: v.binding },
      { key: 'iot.lifetime',   value: v.lifetime },
      { key: 'iot.observable', value: v.observable },
    ]).subscribe({
      next: (r) => { this.saving = false; this.msg = r.ok ? 'Saved.' : 'Error: ' + r.err; },
      error: () => { this.saving = false; this.msg = 'Save failed.'; }
    });
  }
}
