IF OBJECT_ID(N'dbo.sdgo_OperatorProfiles', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorProfiles (
    OperatorID INT NOT NULL PRIMARY KEY,
    CategoryID INT NOT NULL,
    Enabled BIT NOT NULL,
    DeliveryMode VARCHAR(16) NOT NULL,
    DisplayName NVARCHAR(100) NOT NULL,
    Revision CHAR(64) NOT NULL,
    CONSTRAINT CK_sdgo_OperatorDelivery CHECK (DeliveryMode IN ('all_players','product'))
);
IF OBJECT_ID(N'dbo.sdgo_OperatorRules', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorRules (
    OperatorID INT NOT NULL,
    RuleIndex INT NOT NULL,
    CategoryID INT NOT NULL,
    ChanceBps INT NOT NULL CHECK (ChanceBps BETWEEN 0 AND 10000),
    MinAmount INT NOT NULL,
    MaxAmount INT NOT NULL,
    PRIMARY KEY (OperatorID,RuleIndex),
    CHECK (MinAmount BETWEEN 1 AND 65535 AND MaxAmount BETWEEN MinAmount AND 65535)
);
IF COL_LENGTH(N'dbo.sdgo_OperatorProfiles',N'FixedName') IS NULL
    ALTER TABLE dbo.sdgo_OperatorProfiles ADD FixedName BIT NOT NULL
        CONSTRAINT DF_sdgo_OperatorFixedName DEFAULT 0;
IF COL_LENGTH(N'dbo.sdgo_OperatorProfiles',N'PilotExpPercent') IS NULL
    ALTER TABLE dbo.sdgo_OperatorProfiles ADD PilotExpPercent INT NOT NULL
        CONSTRAINT DF_sdgo_OperatorPilotExpPercent DEFAULT 0;
IF COL_LENGTH(N'dbo.sdgo_OperatorProfiles',N'AbilityExtensions') IS NULL
    ALTER TABLE dbo.sdgo_OperatorProfiles ADD AbilityExtensions NVARCHAR(MAX) NOT NULL
        CONSTRAINT DF_sdgo_OperatorAbilityExtensions DEFAULT N'{}';
IF OBJECT_ID(N'dbo.sdgo_OperatorFixedNameBackup',N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorFixedNameBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNumber INT NOT NULL,
    OperatorNickname VARCHAR(16) NOT NULL,
    IsSetName TINYINT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorProcedureBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorProcedureBackup (
    ProcedureName SYSNAME NOT NULL PRIMARY KEY,
    OriginalDefinition NVARCHAR(MAX) NOT NULL,
    InstalledHash CHAR(64) NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorRuleItems', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorRuleItems (
    OperatorID INT NOT NULL,
    RuleIndex INT NOT NULL,
    ItemIndex INT NOT NULL,
    CategoryID INT NOT NULL,
    PRIMARY KEY(OperatorID,RuleIndex,ItemIndex),
    UNIQUE(OperatorID,RuleIndex,CategoryID)
);
IF OBJECT_ID(N'dbo.sdgo_OperatorLegacyNameBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorLegacyNameBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNickname VARCHAR(16) NOT NULL,
    IsSetName TINYINT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorLegacyEntitlementBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorLegacyEntitlementBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNumber INT NOT NULL,
    OPType TINYINT NOT NULL,
    RunOut TINYINT NOT NULL,
    DateEnd DATETIME NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorDataBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorDataBackup (
    OperatorID INT NOT NULL PRIMARY KEY,
    WasPresent BIT NOT NULL,
    OriginalDropType TINYINT NULL,
    BaselineTemplateID INT NULL,
    BackupComplete BIT NOT NULL,
    OriginalOperatorName VARCHAR(100) NULL,
    OriginalTeamWin INT NULL,
    OriginalTeamLose INT NULL,
    OriginalBattleResult INT NULL,
    OriginalChangeSp INT NULL,
    OriginalServiceType INT NULL,
    OriginalAddOn1 INT NULL,
    OriginalAddOn2 INT NULL,
    OriginalAddOn3 INT NULL,
    OriginalAddOn4 INT NULL,
    OriginalGParts INT NULL
);
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'BaselineTemplateID') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD BaselineTemplateID INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'BackupComplete') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD BackupComplete BIT NOT NULL
        CONSTRAINT DF_sdgo_OperatorDataBackupComplete DEFAULT 0;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalOperatorName') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalOperatorName VARCHAR(100) NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalTeamWin') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalTeamWin INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalTeamLose') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalTeamLose INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalBattleResult') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalBattleResult INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalChangeSp') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalChangeSp INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalServiceType') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalServiceType INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalAddOn1') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalAddOn1 INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalAddOn2') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalAddOn2 INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalAddOn3') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalAddOn3 INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalAddOn4') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalAddOn4 INT NULL;
IF COL_LENGTH(N'dbo.sdgo_OperatorDataBackup',N'OriginalGParts') IS NULL
    ALTER TABLE dbo.sdgo_OperatorDataBackup ADD OriginalGParts INT NULL;
IF OBJECT_ID(N'dbo.sdgo_OperatorSkillRegistry', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorSkillRegistry (
    SkillNo INT NOT NULL PRIMARY KEY,
    SkillType INT NOT NULL,
    PointAdd INT NOT NULL,
    UnitExpAdd INT NOT NULL,
    EnergySave INT NOT NULL,
    CHECK (SkillNo > 27),
    CHECK (SkillType = 1)
);
IF OBJECT_ID(N'dbo.sdgo_OperatorTimeRegistry', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorTimeRegistry (
    TimeID INT NOT NULL PRIMARY KEY,
    AddOnID INT NOT NULL UNIQUE,
    OperatorSkill INT NOT NULL,
    StartTime INT NOT NULL,
    CloseTime INT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorNativeCapabilities', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorNativeCapabilities (
    CapabilityName VARCHAR(40) NOT NULL PRIMARY KEY,
    Verified BIT NOT NULL,
    Evidence NVARCHAR(256) NULL
);
IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorNativeCapabilities WHERE CapabilityName='custom_skill_ids')
    INSERT dbo.sdgo_OperatorNativeCapabilities VALUES('custom_skill_ids',0,NULL);
IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorNativeCapabilities WHERE CapabilityName='custom_time_ids')
    INSERT dbo.sdgo_OperatorNativeCapabilities VALUES('custom_time_ids',0,NULL);
IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorNativeCapabilities WHERE CapabilityName='pilot_exp_percent')
    INSERT dbo.sdgo_OperatorNativeCapabilities VALUES('pilot_exp_percent',0,NULL);
IF OBJECT_ID(N'dbo.sdgo_OperatorAbilityExtensions', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorAbilityExtensions (
    ExtensionName VARCHAR(64) NOT NULL PRIMARY KEY,
    SchemaVersion INT NOT NULL,
    Enabled BIT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorItemRegistry', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorItemRegistry (
    CategoryID INT NOT NULL PRIMARY KEY,
    OperatorID INT NOT NULL,
    OwnedCategory BIT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorMissionPartsBackup',N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorMissionPartsBackup (
    OperatorID INT NOT NULL PRIMARY KEY,
    OriginalGParts INT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorGrantIssues', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorGrantIssues (
    AccountNumber INT NOT NULL,
    OperatorID INT NOT NULL,
    ReturnCode INT NOT NULL,
    LastAttempt DATETIME NOT NULL,
    PRIMARY KEY(AccountNumber,OperatorID)
);
