import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { FormBuilder, FormGroup } from '@angular/forms';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

@Component({
  selector: 'app-lwm2m-config',
  templateUrl: './lwm2m-config.component.html',
  styleUrls: ['./lwm2m-config.component.scss']
})
export class Lwm2mConfigComponent implements OnInit, OnDestroy {
  dmForm: FormGroup;
  loading = true; saving = false;
  private sub = new Subscription();
  private readonly KEYS = ['cloud.dm.uri', 'cloud.dm.lifetime', 'cloud.dm.binding'];

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService,
    private ds: DataStoreService
  ) {
    // Only the keys the DM server actually consumes when building the
    // Security/Server TLVs at /bs: cloud.dm.uri, cloud.dm.lifetime,
    // cloud.dm.binding. Per-device DM PSK identity/key are minted
    // per-endpoint into cloud.endpoint.credentials — see the Endpoints page.
    this.dmForm = fb.group({
      dm_uri:        ['coaps://0.0.0.0:5683'],
      lifetime:      [90],
      binding:       ['U'],
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
        if (!this.dmForm.dirty) this.applyData(this.ds.snapshot());
      }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    this.dmForm.patchValue({
      dm_uri:   d['cloud.dm.uri']      || 'coaps://0.0.0.0:5683',
      lifetime: d['cloud.dm.lifetime'] || 90,
      binding:  d['cloud.dm.binding']  || 'U',
    });
  }

  save(): void {
    this.saving = true;
    const v = this.dmForm.value;
    this.ds.write([
      { key: 'cloud.dm.uri',              value: v.dm_uri },
      { key: 'cloud.dm.lifetime',         value: v.lifetime },
      { key: 'cloud.dm.binding',          value: v.binding },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('DM config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
