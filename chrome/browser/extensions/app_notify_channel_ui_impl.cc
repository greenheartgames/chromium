// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/app_notify_channel_ui_impl.h"

#include "base/utf_string_conversions.h"
#include "chrome/browser/api/infobars/confirm_infobar_delegate.h"
#include "chrome/browser/api/infobars/infobar_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/signin_manager.h"
#include "chrome/browser/signin/signin_manager_factory.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/webui/signin/login_ui_service.h"
#include "chrome/browser/ui/webui/signin/login_ui_service_factory.h"
#include "chrome/common/url_constants.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"

namespace extensions {

namespace {

class AppNotifyChannelUIInfoBarDelegate : public ConfirmInfoBarDelegate {
 public:
  // Creates an app notify channel UI delegate and adds it to |infobar_service|.
  static void Create(InfoBarService* infobar_service,
                     AppNotifyChannelUIImpl* creator,
                     const std::string& app_name);

  // ConfirmInfoBarDelegate.
  virtual string16 GetMessageText() const OVERRIDE;
  virtual string16 GetButtonLabel(InfoBarButton button) const OVERRIDE;
  virtual bool Accept() OVERRIDE;
  virtual bool Cancel() OVERRIDE;
  virtual void InfoBarDismissed() OVERRIDE;

 private:
  AppNotifyChannelUIInfoBarDelegate(AppNotifyChannelUIImpl* creator,
                                    InfoBarService* infobar_service,
                                    const std::string& app_name);
  virtual ~AppNotifyChannelUIInfoBarDelegate();

  AppNotifyChannelUIImpl* creator_;
  std::string app_name_;

  DISALLOW_COPY_AND_ASSIGN(AppNotifyChannelUIInfoBarDelegate);
};

// static
void AppNotifyChannelUIInfoBarDelegate::Create(InfoBarService* infobar_service,
                                               AppNotifyChannelUIImpl* creator,
                                               const std::string& app_name) {
  infobar_service->AddInfoBar(scoped_ptr<InfoBarDelegate>(
      new AppNotifyChannelUIInfoBarDelegate(creator, infobar_service,
                                            app_name)));
}

string16 AppNotifyChannelUIInfoBarDelegate::GetMessageText() const {
  return l10n_util::GetStringFUTF16(IDS_APP_NOTIFICATION_NEED_SIGNIN,
                                    UTF8ToUTF16(app_name_));
}

string16 AppNotifyChannelUIInfoBarDelegate::GetButtonLabel(
    InfoBarButton button) const {
  if (button == BUTTON_OK) {
    return l10n_util::GetStringUTF16(IDS_APP_NOTIFICATION_NEED_SIGNIN_ACCEPT);
  } else if (button == BUTTON_CANCEL) {
    return l10n_util::GetStringUTF16(IDS_APP_NOTIFICATION_NEED_SIGNIN_CANCEL);
  } else {
    NOTREACHED();
  }
  return string16();
}

bool AppNotifyChannelUIInfoBarDelegate::Accept() {
  creator_->OnInfoBarResult(true);
  return true;
}

bool AppNotifyChannelUIInfoBarDelegate::Cancel() {
  creator_->OnInfoBarResult(false);
  return true;
}

void AppNotifyChannelUIInfoBarDelegate::InfoBarDismissed() {
  Cancel();
}

AppNotifyChannelUIInfoBarDelegate::AppNotifyChannelUIInfoBarDelegate(
    AppNotifyChannelUIImpl* creator,
    InfoBarService* infobar_service,
    const std::string& app_name)
    : ConfirmInfoBarDelegate(infobar_service),
      creator_(creator),
      app_name_(app_name) {
}

AppNotifyChannelUIInfoBarDelegate::~AppNotifyChannelUIInfoBarDelegate() {
}

}  // namespace


AppNotifyChannelUIImpl::AppNotifyChannelUIImpl(
    Profile* profile,
    content::WebContents* web_contents,
    const std::string& app_name,
    AppNotifyChannelUI::UIType ui_type)
    : profile_(profile->GetOriginalProfile()),
      web_contents_(web_contents),
      app_name_(app_name),
      ui_type_(ui_type),
      delegate_(NULL),
      observing_sync_(false),
      wizard_shown_to_user_(false) {
}

AppNotifyChannelUIImpl::~AppNotifyChannelUIImpl() {
  // We should have either not started observing sync, or already called
  // StopObservingSync by this point.
  CHECK(!observing_sync_);
}

// static
AppNotifyChannelUI* AppNotifyChannelUI::Create(Profile* profile,
    content::WebContents* web_contents,
    const std::string& app_name,
    AppNotifyChannelUI::UIType ui_type) {
  return new AppNotifyChannelUIImpl(profile, web_contents, app_name, ui_type);
}

void AppNotifyChannelUIImpl::PromptSyncSetup(
    AppNotifyChannelUI::Delegate* delegate) {
  CHECK(delegate_ == NULL);
  delegate_ = delegate;

  if (!ProfileSyncServiceFactory::GetInstance()->HasProfileSyncService(
          profile_)) {
    delegate_->OnSyncSetupResult(false);
    return;
  }

  if (ui_type_ == NO_INFOBAR) {
    OnInfoBarResult(true);
    return;
  }

  AppNotifyChannelUIInfoBarDelegate::Create(
      InfoBarService::FromWebContents(web_contents_), this, app_name_);
}

void AppNotifyChannelUIImpl::OnInfoBarResult(bool accepted) {
  if (!accepted) {
    delegate_->OnSyncSetupResult(false);
    return;
  }

  StartObservingSync();
  // Bring up the login page.
  LoginUIService* login_ui_service =
      LoginUIServiceFactory::GetForProfile(profile_);
  LoginUIService::LoginUI* login_ui = login_ui_service->current_login_ui();
  if (login_ui) {
    // Some sort of login UI is already visible.
    SigninManager* signin = SigninManagerFactory::GetForProfile(profile_);
    if (signin->GetAuthenticatedUsername().empty()) {
      // User is not logged in yet, so just bring up the login UI (could be
      // the promo UI).
      login_ui->FocusUI();
      return;
    } else {
      // User is already logged in, so close whatever sync config UI the
      // user is looking at and display new login UI.
      login_ui->CloseUI();
      DCHECK(!login_ui_service->current_login_ui());
    }
  }
  // Any existing UI is now closed - display new login UI.
  Browser* browser = chrome::FindBrowserWithWebContents(web_contents_);
  chrome::ShowSettingsSubPage(browser, chrome::kSyncSetupForceLoginSubPage);
}

void AppNotifyChannelUIImpl::OnStateChanged() {
  ProfileSyncService* sync_service =
      ProfileSyncServiceFactory::GetInstance()->GetForProfile(profile_);
  LoginUIService* login_service =
      LoginUIServiceFactory::GetForProfile(profile_);

  bool wizard_visible = (login_service->current_login_ui() != NULL);
  // ProfileSyncService raises OnStateChanged many times. Even multiple
  // times before the wizard actually becomes visible for the first time.
  // So we have to wait for the wizard to become visible once and then we
  // wait for it to get dismissed.
  bool finished = wizard_shown_to_user_ && !wizard_visible;
  if (wizard_visible)
    wizard_shown_to_user_ = true;

  if (finished) {
    StopObservingSync();
    delegate_->OnSyncSetupResult(sync_service->HasSyncSetupCompleted());
  }
}

void AppNotifyChannelUIImpl::StartObservingSync() {
  CHECK(!observing_sync_);
  observing_sync_ = true;
  ProfileSyncServiceFactory::GetInstance()->GetForProfile(
      profile_)->AddObserver(this);
}

void AppNotifyChannelUIImpl::StopObservingSync() {
  CHECK(observing_sync_);
  observing_sync_ = false;
  ProfileSyncServiceFactory::GetInstance()->GetForProfile(
      profile_)->RemoveObserver(this);
}

}  // namespace extensions
