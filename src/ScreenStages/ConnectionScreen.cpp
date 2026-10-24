// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2016 Hotride

#include "ConnectionScreen.h"
#include "../CrossUO.h"
#include "../CharacterList.h"
#include "../Managers/ScreenEffectManager.h"
#include "../Network/Packets.h"
#include "../Managers/ClilocManager.h"
#include "../Globals.h" // g_Language

CConnectionScreen g_ConnectionScreen;

CConnectionScreen::CConnectionScreen()
    : CBaseScreen(m_ConnectionGump)
{
}

CConnectionScreen::~CConnectionScreen()
{
}

void CConnectionScreen::Init()
{
    CBaseScreen::Init();
    m_Text = "";
    m_ConnectionFailed = false;
    m_Connected = false;
    m_Completed = false;
    m_ErrorCode = 0;
    m_Type = CST_LOGIN;

    g_ScreenEffectManager.UseSunrise();
    SmoothScreenAction = 0;

    m_Gump.PrepareTextures();
    m_Gump.WantUpdateContent = true;
}

astr_t CConnectionScreen::LoginMessage() const
{
    if (!m_Text.empty())
    {
        return m_Text;
    }
    auto cliloc = g_ClilocManager.Cliloc(g_Language);
    if (!m_ConnectionFailed)
    {
        return m_Connected ? cliloc->GetA(3000003, false, "Verifying Account...") :
                             cliloc->GetA(3000002, false, "Connecting...");
    }
    switch (m_ErrorCode)
    {
        case 0:
            return cliloc->GetA(3000007, false, "Incorrect name/password.");
        case 1:
            return cliloc->GetA(3000034, false, "Someone is already using this account.");
        case 2:
            return cliloc->GetA(3000035, false, "Your account has been blocked.");
        case 3:
            return cliloc->GetA(3000036, false, "Your account credentials are invalid.");
        case 4:
            return "Communication problem.";
        case 5:
            return "The IGR concurrency limit has been met.";
        case 6:
            return "The IGR time limit has been met.";
        case 7:
            return "General IGR authentication failure.";
        default:
            return cliloc->GetA(
                3000037, false, "Couldn't connect to Ultima Online.  Please try again in a few moments.");
    }
}

void CConnectionScreen::SetConnectionFailed(bool val)
{
    m_ConnectionFailed = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetConnected(bool val)
{
    m_Connected = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetCompleted(bool val)
{
    m_Completed = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetErrorCode(int val)
{
    m_ErrorCode = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetType(CONNECTION_SCREEN_TYPE val)
{
    m_Type = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetTextA(const astr_t &val)
{
    m_Text = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::ProcessSmoothAction(uint8_t action)
{
    if (action == 0xFF)
    {
        action = SmoothScreenAction;
    }

    if (action == ID_SMOOTH_CS_GO_SCREEN_MAIN)
    {
        g_Game.InitScreen(GS_MAIN);
    }
    else if (action == ID_SMOOTH_CS_GO_SCREEN_CHARACTER)
    {
        g_Game.InitScreen(GS_CHARACTER);
    }
    else if (action == ID_SMOOTH_CS_GO_SCREEN_PROFESSION)
    {
        g_Game.InitScreen(GS_PROFESSION_SELECT);
    }
    else if (action == ID_SMOOTH_CS_SEND_DELETE)
    {
        CPacketDeleteCharacter(g_CharacterList.Selected).Send();
    }
}

void CConnectionScreen::OnKeyDown(const KeyEvent &ev)
{
    const auto key = EvKey(ev);
    switch (key)
    {
        case KEY_RETURN:
        case KEY_RETURN2:
        {
            if (m_Type == CST_CHARACTER_LIST)
            {
                CreateSmoothAction(ID_SMOOTH_CS_SEND_DELETE);
            }
            else if (m_Type == CST_SELECT_PROFESSOIN)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_PROFESSION);
            }
            else if (m_Type == CST_GAME)
            {
                if (m_ErrorCode > 0)
                {
                    CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
                }
            }
            else if (m_Type == CST_CONLOST || m_ConnectionFailed)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_MAIN);
            }

            break;
        }
        case KEY_ESCAPE:
        {
            if (m_Type == CST_CHARACTER_LIST)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
            }
            else if (m_Type == CST_SELECT_PROFESSOIN)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_PROFESSION);
            }
            else if (m_Type == CST_GAME)
            {
                if (m_ErrorCode > 0)
                {
                    CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
                }
            }
            else if (m_Type == CST_CONLOST || m_ConnectionFailed)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_MAIN);
            }

            break;
        }
        default:
            break;
    }
}
